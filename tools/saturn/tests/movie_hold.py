#!/usr/bin/env python3
"""Mednafen regression: release briefing playback while retaining its last image.

Requires the normal Saturn build (no SABER.ENV shortcuts) and debug kit.
"""
from pathlib import Path
import hashlib
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from mednafen_run import KIT, ROOT, LogRing, Mednafen, symbols

_, _, syms = symbols(ROOT / 'obj-saturn/saber_rider.elf')
def symbol(name):
    matches = [a for n, a in syms.items() if n == '_' + name or n.startswith('_' + name + '.lto_priv.')]
    assert len(matches) == 1, (name, matches)
    return matches[0]

emu = Mednafen(KIT, None)
def read(addr, size):
    space, off = ('workramh', addr - 0x06000000) if addr >= 0x06000000 else ('workraml', addr - 0x00200000)
    return bytes.fromhex(emu.call('mem_read', space, off, size))
def word(name):
    return int.from_bytes(read(symbol(name), 4), 'big')
def flag(name):
    return read(symbol(name), 1)[0]
def image():
    return bytes.fromhex(emu.call('mem_read', 'vdp2vram', 0, 3 * 0x20000))

try:
    emu.call('load', ROOT / 'build/saturn/saber_rider.cue', 'ss')
    ring = LogRing(emu, symbol('saber_log'))
    frame = 0
    def run(to):
        global frame
        emu.call('run', to - frame, 1)
        frame = to
        for line in ring.read():
            print(f'{frame}: {line}', flush=True)
    # Skip intro, then enter the briefing without advancing its dialogue.
    for at, buttons in [(1100, 'start'), (1110, 'none'), (1400, 'start'), (1410, 'none')]:
        run(at)
        emu.call('pad', 0, buttons)
    run(1600)
    assert word('vid_hook') and word('vid_ud'), 'briefing did not start'
    assert not flag('driver_ok'), 'movie must own the sound hardware while playing'
    for _ in range(300):
        run(frame + 10)
        if not word('vid_hook'):
            break
    else:
        raise AssertionError('briefing never released playback')
    # A frame stop can interrupt the synchronous driver upload after the
    # hook has detached. Allow the handoff to finish before inspecting it.
    run(frame + 10)
    assert word('vid_state') == 2, 'last frame surface must stay READY'
    assert word('vid_ud') == 0
    assert flag('driver_ok'), 'game sound driver must resume at movie end'
    last = image()
    assert len(set(last)) > 2, 'retained frame must contain the movie image'
    run(frame + 120)
    assert word('vid_state') == 2 and not word('vid_hook')
    assert image() == last, 'last movie image changed after playback ended'
    assert flag('driver_ok')
    print('PASS: natural briefing end restores game audio, detaches decoding, and retains identical RGB24 VDP2 pixels for 120 fields')
    print('Retained image SHA256:', hashlib.sha256(last).hexdigest())
finally:
    emu.close()
