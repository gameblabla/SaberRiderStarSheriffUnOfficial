#!/usr/bin/env python3
"""Re-run the captured stage-7 dialog cache allocation natively.

The fixture comes from the first refused required sprite in the SGX campaign.
This calls the real banked C allocator body with its captured globals seeded;
no host-side allocator model is used.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

from emulator import Emulator, boot, symbol, ROOT

FIXTURE = Path(__file__).resolve().parent / 'fixtures' / 'sgx_stage7_cache_pressure.json'
TRAMPOLINE = 0x3BF0
STAGE_OFFSET = 7  # PceTelemetry: magic[4], version, ready, ports, stage


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def bytes_from_hex(value):
    return bytes.fromhex(value)


def u16(data, index):
    return struct.unpack_from('<H', data, index * 2)[0]


def verify(out, fixture_path, expect_refused=False, report_path=None):
    out = out.resolve()
    fixture = json.loads(fixture_path.read_text())
    capture = fixture['capture']
    seed = fixture['seed']
    app = out / 'app.elf'
    cue = out / 'saber_rider.cue'
    assert app.is_file() and cue.is_file(), f'Missing PCE output in {out}'
    app_hash = sha256(app)
    iso_hash = sha256(out / 'saber_rider.iso')
    if expect_refused:
        assert app_hash == capture['app_sha256'], ('--expect-refused requires the frozen failing app', app_hash, capture['app_sha256'])
        assert iso_hash == capture['iso_sha256'], ('--expect-refused requires the frozen failing ISO', iso_hash, capture['iso_sha256'])

    names = [
        'pce_metrics', 'pce_sgx_metrics', 'sprite_cache_stage',
        'sprite_cache_foreground_first', 'herd_on', 'sat_page', 'sprite_last_free',
        'sprite_epoch', 'sprite_stamp', 'cache_id', 'cache_count', 'cache_result',
        'sprite_ids', 'sprite_words', 'sprite_count', 'sprite_used', 'sprite_pinned',
        'sprite_slot_of', 'pattern_owner', 'sprite_pb_hi',
    ]
    addr = {name: symbol(app, name) for name in names}
    initial = {name: bytes_from_hex(seed['arrays'][name]) for name in seed['arrays']}
    for name, data in initial.items():
        expected = {'sprite_ids':96, 'sprite_words':96, 'sprite_count':48,
                    'sprite_used':48, 'sprite_pinned':48, 'sprite_slot_of':512,
                    'pattern_owner':54, 'sprite_pb_hi':48}[name]
        assert len(data) == expected, (name, len(data), expected)

    with tempfile.TemporaryDirectory(prefix='sgx-cache-pressure-', dir=out) as base, Emulator(cue, base, sgx=True) as e:
        boot(e, addr['pce_metrics'])
        e.run(120)

        # Recreate the failing stage/cache context. Stage 7 is the only metric
        # field this body needs; SGX gameplay flags are copied from the capture.
        e.write(addr['pce_metrics'] + STAGE_OFFSET, bytes([seed['stage']]))
        e.write(addr['pce_sgx_metrics'], bytes_from_hex(seed['pce_sgx_metrics_raw_hex']))
        e.write(addr['sprite_cache_stage'], bytes([seed['sprite_cache_stage']]))
        e.write(addr['sprite_cache_foreground_first'], struct.pack('<H', seed['sprite_cache_foreground_first']))
        e.write(addr['herd_on'], bytes([seed['herd_on']]))
        e.write(addr['sat_page'], bytes([seed['sat_page']]))
        e.write(addr['sprite_last_free'], bytes([seed['sprite_last_free']]))
        e.write(addr['sprite_epoch'], bytes([seed['sprite_epoch']]))
        e.write(addr['sprite_stamp'], bytes_from_hex(seed['sprite_stamp_hex']))
        for name, data in initial.items():
            e.write(addr[name], data)
        e.write(addr['cache_id'], struct.pack('<H', seed['cache_id']))
        e.write(addr['cache_count'], struct.pack('<H', seed['cache_count']))
        e.write(addr['cache_result'], b'\xaa\xaa')

        # JSR allocate; JMP back to itself after RTS. JSR pushes $3bf2 and RTS
        # returns to $3bf3. MPR3 maps the banked allocator body; P=4 masks IRQs.
        allocator = next(int(line.split()[0],16) for line in subprocess.check_output(
            [str(ROOT/'PCE/llvm-mos8/bin/llvm-nm'),str(app)],text=True).splitlines()
            if len(line.split())==3 and line.split()[2]=='allocate')
        body = allocator & 0xFFFF
        bank = (allocator >> 16) & 255
        e.write(TRAMPOLINE, bytes([0x20, body & 0xFF, body >> 8, 0x4C, 0xF3, 0x3B]))
        for key, value in [('P', 4), ('SP', 253), ('MPR3', bank), ('MPR6', 108), ('PC', TRAMPOLINE)]:
            e.call('register_set', key, value)
        e.run(1)

        result = struct.unpack('<H', e.memory(addr['cache_result'], 2))[0]
        after = {name: e.memory(addr[name], len(data)) for name, data in initial.items()}
        if expect_refused:
            assert result == fixture['expected']['refused']['cache_result'], result
            assert after['pattern_owner'] == initial['pattern_owner'], 'Refused allocator call must not alter ownership'
            assert after['sprite_words'] == initial['sprite_words'], 'Refused allocator call must not alter words'
        else:
            expected = fixture['expected']['success']
            slot = expected['cache_result']
            assert result == slot, ('native allocator result', result, slot)
            assert u16(after['sprite_words'], slot) == expected['sprite_words_slot8'], ('native word placement', hex(u16(after['sprite_words'], slot)))
            assert list(after['pattern_owner'][48:54]) == expected['pattern_owner_pages_48_to_53'], after['pattern_owner'][48:54]
            assert after['pattern_owner'][:48] == initial['pattern_owner'][:48], 'Original protected pages must stay untouched'
            assert after['sprite_ids'] == initial['sprite_ids'], 'Allocator body reserves a slot but does not publish its sprite ID'
        regs = e.call('registers')['registers']
        assert regs['MPR3'] == bank and regs['MPR6'] == 108, regs

    result_obj = {
        'passed': True,
        'mode': 'expect_refused' if expect_refused else 'expect_allocated',
        'out': str(out),
        'app_sha256': app_hash,
        'iso_sha256': iso_hash,
        'fixture_capture_app_sha256': capture['app_sha256'],
        'allocator_symbol_cpu_address': f'${body:04x}',
        'invocation': {'trampoline': '$3bf0', 'P': 4, 'SP': 253, 'MPR3': bank, 'MPR6': 108, 'frames': 1},
        'sprite_id': seed['cache_id'],
        'sprite_patterns': seed['cache_count'],
        'sp_after_native_call': regs['SP'],
        'cache_result': result,
        'sprite_words_slot8': u16(after['sprite_words'], 8),
        'pattern_owner_pages_48_to_53': list(after['pattern_owner'][48:54]),
        'protected_pages_0_to_47_unchanged': after['pattern_owner'][:48] == initial['pattern_owner'][:48],
    }
    if report_path is None:
        report_path = out / 'sgx-cache-pressure-verification.json'
    report_path = report_path.resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(result_obj, indent=2) + '\n')
    print(json.dumps(result_obj, indent=2))
    return result_obj


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=Path('build/sgx'))
    parser.add_argument('--fixture', type=Path, default=FIXTURE)
    parser.add_argument('--expect-refused', action='store_true', help='Verify the frozen pre-fix app refuses the captured allocation')
    parser.add_argument('--report', type=Path, help='Write JSON report here; defaults to OUT/sgx-cache-pressure-verification.json')
    args = parser.parse_args()
    verify(args.out, args.fixture.resolve(), args.expect_refused, args.report)
