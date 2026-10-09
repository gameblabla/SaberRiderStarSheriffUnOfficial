#!/usr/bin/env python3
"""Native CD-DA start/change/cancel/loop tests, independent of the debug menu."""
import argparse
import json
import tempfile
import wave
from pathlib import Path
from urllib.parse import quote

import numpy as np
from emulator import Emulator, boot, symbol


def verify(out):
    elf = out/'app.elf'
    names = 'pce_metrics audio_music audio_music_once audio_stop audio_tick pce_music_status'.split()
    a = {name: symbol(elf, name) for name in names}
    options = symbol(elf, 'pce_options.0')
    review = out/'cdda-review'
    review.mkdir(exist_ok=True)
    report = {}

    with tempfile.TemporaryDirectory(prefix='cdda-', dir=out) as base, Emulator(out/'saber_rider.cue', base) as e:
        boot(e, a['pce_metrics'])

        def call(name, arg=0, ports=()):
            address = a[name]
            tick = a['audio_tick']
            # After the call, keep polling music with the real VBlank, raster
            # and PCM IRQs. Gameplay cannot replace the isolated request.
            # Debugger high-level memory pokes bypass I/O handlers. Seed
            # hardware ports with real CPU stores before issuing the request.
            prefix = b''.join(bytes([0xa9, value, 0x8d, port & 255, port >> 8])
                              for port, value in ports)
            loop = 0x3bfa + len(prefix)
            code = prefix + bytes([0xa9, arg, 0x20, address & 255, address >> 8,
                          0xa9, 1, 0x8d, 0xe0, 0x3b,
                          0x20, tick & 255, tick >> 8, 0x4c, loop & 255, loop >> 8])
            e.write(0x3be0, b'\0')
            e.write(0x3bf0, code)
            for key, value in [('P', 0), ('SP', 253), ('MPR3', 105), ('MPR6', 108), ('PC', 0x3bf0)]:
                e.call('register_set', key, value)
            # Stop can wait across several VBlanks for a cancelled seek.
            # Check its postconditions only after the native call returns.
            for _ in range(900):
                e.run(1)
                if e.memory(0x3be0,1)==b'\1':break
            else:raise AssertionError(('Native audio call did not return',name))

        def bus():
            return e.memory(0x1800, 1)[0]

        def settled(limit=900):
            for _ in range(limit):
                if e.memory(a['pce_music_status'], 1) == b'\0' and not bus() & 0x80:
                    return
                e.run(1)
            raise AssertionError(('CD-DA did not settle', e.memory(a['pce_music_status'], 1).hex(), hex(bus())))

        def capture(name, frames=90):
            path = review/(name+'.wav')
            e.call('sound_capture', frames, quote(str(path.resolve()), safe='/'))
            with wave.open(str(path), 'rb') as wav:
                pcm = np.frombuffer(wav.readframes(wav.getnframes()), '<i2').astype(np.float64)
            return float(np.sqrt(np.mean(pcm*pcm)))

        call('audio_stop')
        assert not bus() & 0x80
        # Preserve independently enabled IFU bits through every ACK edge.
        for level in (3, 2, 1):
            e.write(options+3, bytes([level]))
            call('audio_music', 5, ports=((0x1802,0x10),(0x180f,0x0c)))
            settled()
            e.run(180)
            rms = capture(f'stage1-volume-{level}')
            assert rms > 20, (level, rms, 'CD-DA must produce sound')
            assert e.memory(0x1802, 1)[0] & 0x10
            assert e.memory(0x180f, 1) == b'\0'
            report[f'volume_{level}_rms'] = rms

        e.write(options+3, b'\3')
        # Replace a pending cue before its seek response. No cancelled D9 may
        # retain ownership while the second track is being selected.
        call('audio_music', 0)
        call('audio_music', 17)
        settled()
        e.run(180)
        assert capture('changed-track') > 20
        call('audio_music', 1)
        call('audio_stop')
        assert not bus() & 0x80, 'Stop must return with the SCSI bus free'
        e.run(180)
        assert capture('cancelled-seek', 30) < 10
        report['change_and_cancel'] = 'passed'

        # Track 4 is the short credits jingle. Wait past its actual extent to
        # distinguish native one-shot stopping from native repeat restart.
        track = next(t for t in json.loads((out/'audio.json').read_text())['tracks'] if t['track'] == 6)
        duration = (out/track['file']).stat().st_size//2352-150
        frames = duration*60//75+300
        call('audio_music_once', 4)
        settled()
        e.run(frames)
        assert capture('one-shot-ended', 30) < 10
        call('audio_music', 4)
        settled()
        e.run(frames)
        assert capture('repeat-restarted', 90) > 20
        report['one_shot_and_repeat'] = 'passed'
        call('audio_stop')
        e.write(options+3, b'\0')
        call('audio_music', 5)
        assert not bus() & 0x80
        # The mixer retains a short filter tail immediately after stopping.
        # Allow it to drain before measuring the disabled-music steady state.
        e.run(30)
        assert capture('music-off', 30) < 10
        report['off'] = 'silent; bus free'

    (out/'cdda-verification.json').write_text(json.dumps(report, indent=2)+'\n')
    print(report)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('--out', type=Path, default=Path('build/pce'))
    verify(p.parse_args().out.resolve())
