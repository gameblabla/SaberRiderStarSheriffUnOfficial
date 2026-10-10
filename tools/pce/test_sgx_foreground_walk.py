#!/usr/bin/env python3
"""Exercise SGX foreground admission through the supplied save's camera window."""
import argparse,json,struct
from pathlib import Path
from emulator import Emulator,boot,symbol
from test_sgx_rendering import Rendering

def verify(out):
    out=out.resolve();render=Rendering(out);reports=[];failures=[]
    with Emulator(out/'saber_rider.cue',out/'foreground-walk-emulator',sgx=True) as e:
        boot(e,symbol(out/'app.elf','pce_metrics'));render.dialogs(e);e.run(120)
        hero_address=symbol(out/'app.elf','hero')
        try:control_hero=symbol(out/'app.elf','pce_control.3')
        except ValueError:control_hero=None
        scene=render.manifest['scenes'][0];blob=(out/'s1.bin').read_bytes()
        source_offsets={1:(scene['sgx_foreground_offset'],scene['sgx_foreground_count']),
                        0:(scene['foreground_slow_offset'],scene['foreground_slow_count'])}
        cameras=[*range(8416,8490,8),8489]
        for step,camera in enumerate(cameras):
            player_x=camera+120
            if control_hero is not None:e.write(control_hero,b'\x02')
            e.write(hero_address,b'\x02')
            e.write(render.sym['actors'],bytes(168));e.write(render.sym['shots'],bytes(208))
            e.write(render.sym['player'],struct.pack('<4h4B',player_x,177,0,0,0,0,4,4))
            e.write(render.sym['camera'],struct.pack('<H',camera))
            render.seed(e,'safe_timer',250,1);render.seed(e,'dialogs_done',255,1)
            e.input(0);e.run(90 if step==0 else 1);render.settle(e)
            metrics=render.metrics(e)
            assert metrics['stage']==1 and metrics['hero']==2 and abs(metrics['camera_x']-camera)<=2,(
                'camera walk did not settle at requested state',camera,metrics)
            retained=render.foreground(e,allow_camera_lag=True)
            row={'camera':metrics['camera_x'],'retained_sat_parts':retained}
            try:
                row.update(render.foreground_source_coverage(e,1))
            except AssertionError as exc:
                detail=exc.args[0]
                missing=detail[4] if isinstance(detail,tuple) and len(detail)>4 else {}
                source_missing=[]
                for (group,index),parts in missing.items():
                    offset,count=source_offsets[group]
                    assert index<count
                    _x,_y,sprite_id=struct.unpack_from('<hhH',blob,offset+index*6)
                    source_missing.append({'group':group,'index':index,'sprite_id':sprite_id,'parts':parts})
                row['coverage_failure']=repr(exc.args)
                row['missing_source_sprites']=source_missing
                failures.append(row.copy())
            reports.append(row)
        e.screenshot(out/'foreground-camera8489-walk.png')
    result={'cameras':reports,'failures':failures,'passed':not failures}
    (out/'foreground-walk-verification.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({'sampled_cameras':[row['camera'] for row in reports],
                      'failure_cameras':[row['camera'] for row in failures],
                      'missing_source_sprites':[{k:v for k,v in row.items() if k in ('camera','missing_source_sprites')}
                                                for row in failures],
                      'passed':not failures},indent=2),flush=True)
    assert not failures,'foreground source chunks were refused during gradual camera advance'

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,default=Path('build/sgx'))
    verify(p.parse_args().out)
