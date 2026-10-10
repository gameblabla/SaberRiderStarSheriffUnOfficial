#!/usr/bin/env python3
"""Exercise the SGX compiler-ABI arithmetic helpers on the emulated CPU."""
import argparse
import json
import random
import struct
import tempfile
from pathlib import Path

from emulator import Emulator, boot, symbol
from test_campaign import Campaign

STUB = 0x3BF0
RESULTS = 0x3A00
MARKER = 0x3BEF
MPR6_SENTINEL = 109
RC_SENTINEL = bytes((0xA5 ^ i) & 0xFF for i in range(16))  # rc16..rc31
ACRAM_START = 0x1D0000


class Code:
    def __init__(self):
        self.data = bytearray()
        self.labels = {}
        self.branches = []

    def emit(self, *values):
        self.data.extend(values)

    def label(self, name):
        self.labels[name] = len(self.data)

    def branch(self, opcode, label):
        self.emit(opcode, 0)
        self.branches.append((len(self.data) - 1, label))

    def lda_imm(self, value):
        self.emit(0xA9, value & 0xFF)

    def ldx_imm(self, value):
        self.emit(0xA2, value & 0xFF)

    def sta(self, address):
        self.emit(0x8D, address & 0xFF, address >> 8)

    def stx(self, address):
        self.emit(0x8E, address & 0xFF, address >> 8)

    def jsr(self, address):
        self.emit(0x20, address & 0xFF, address >> 8)

    def call_mpr6(self, destination):
        # TMA #$40 reads MPR6 after the routine returns.
        self.emit(0x43, 0x40)
        self.sta(destination)

    def finish(self, marker=1):
        self.lda_imm(marker)
        self.sta(MARKER)
        end = STUB + len(self.data)
        self.emit(0x4C, end & 0xFF, end >> 8)
        for offset, label in self.branches:
            delta = self.labels[label] - offset - 1
            assert -128 <= delta <= 127, (label, delta)
            self.data[offset] = delta & 0xFF
        assert STUB + len(self.data) <= 0x4000, len(self.data)
        return bytes(self.data)


def set_arcade_output(code, bank=29):
    # Configure the real Arcade Card port and stream writes to one bank.
    for address, value in ((0x1A32, 0), (0x1A33, 0), (0x1A34, 0),
                           (0x1A35, 0), (0x1A36, 0), (0x1A37, 1),
                           (0x1A38, 0), (0x1A39, 0x11)):
        code.lda_imm(value)
        code.sta(address)
    code.lda_imm(bank); code.sta(0x1A34)


def exhaustive_byte_code(mul8, udiv8, umod8):
    c = Code()
    set_arcade_output(c, 29)
    c.lda_imm(0); c.sta(0x3B90)
    c.sta(0x3B91)
    c.label('mul_outer')
    c.label('mul_inner')
    c.emit(0xAD, 0x90, 0x3B, 0xAE, 0x91, 0x3B)
    c.jsr(mul8)
    c.sta(0x1A30); c.stx(0x1A30)
    c.emit(0xEE, 0x91, 0x3B)
    c.branch(0xD0, 'mul_inner')
    c.emit(0xEE, 0x90, 0x3B)
    c.branch(0xD0, 'mul_outer')
    c.lda_imm(1); c.sta(MARKER)

    set_arcade_output(c, 27)
    c.lda_imm(0); c.sta(0x3B90)
    c.label('div_outer')
    c.lda_imm(1); c.sta(0x3B91)
    c.label('div_inner')
    c.emit(0xAD, 0x90, 0x3B, 0xAE, 0x91, 0x3B)
    c.jsr(udiv8)
    c.sta(0x1A30)
    c.emit(0xAD, 0x90, 0x3B, 0xAE, 0x91, 0x3B)
    c.jsr(umod8)
    c.sta(0x1A30)
    c.emit(0xEE, 0x91, 0x3B)
    c.branch(0xD0, 'div_inner')
    c.emit(0xEE, 0x90, 0x3B)
    c.branch(0xD0, 'div_outer')
    return c.finish(2)


def read_acram(e, address, size):
    return b''.join(bytes.fromhex(e.call('asread', 'acram', address + off,
                                         min(16384, size - off))['hex'])
                    for off in range(0, size, 16384))


def run_raw(e, code, frames, *, batch='arithmetic', expected_marker=1):
    e.write(0x2010, RC_SENTINEL)
    e.write(MARKER, b'\0')
    e.write(STUB, code)
    for register, value in (('P', 4), ('SP', 253), ('MPR2', 104),
                            ('MPR6', MPR6_SENTINEL), ('PC', STUB)):
        e.call('register_set', register, value)
    observed = 0
    for elapsed in range(0, frames, 200):
        e.run(min(200, frames - elapsed))
        observed = e.memory(MARKER, 1)[0]
        if observed == expected_marker:
            break
    assert observed == expected_marker, (batch, 'native arithmetic did not finish',
                                         observed, e.memory(0x3B90, 2).hex(),
                                         e.call('registers')['registers']['PC'])
    regs = e.call('registers')['registers']
    assert regs['MPR6'] == MPR6_SENTINEL, (batch, 'caller MPR6 was not restored', regs['MPR6'])
    actual_rc = e.memory(0x2010, len(RC_SENTINEL))
    assert actual_rc == RC_SENTINEL, (batch, 'rc16..rc31 were modified', actual_rc.hex())


def write_16_operands(c, lhs, rhs):
    c.lda_imm(rhs & 0xFF); c.sta(0x2002)
    c.lda_imm(rhs >> 8); c.sta(0x2003)
    c.lda_imm(lhs & 0xFF); c.ldx_imm(lhs >> 8)


def write_32_operands(c, lhs, rhs):
    for i in range(4):
        c.lda_imm((rhs >> (8 * i)) & 0xFF); c.sta(0x2004 + i)
    c.lda_imm((lhs >> 16) & 0xFF); c.sta(0x2002)
    c.lda_imm((lhs >> 24) & 0xFF); c.sta(0x2003)
    c.lda_imm(lhs & 0xFF); c.ldx_imm((lhs >> 8) & 0xFF)


def append_16_result(c, address):
    c.sta(address); c.stx(address + 1); c.call_mpr6(address + 2)


def append_32_result(c, address):
    c.sta(address); c.stx(address + 1)
    c.emit(0xAD, 0x02, 0x20); c.sta(address + 2)
    c.emit(0xAD, 0x03, 0x20); c.sta(address + 3)
    c.call_mpr6(address + 4)


def batches(items, size):
    for start in range(0, len(items), size):
        yield items[start:start + size]


def test_words(e, elf):
    names = ('pce_fast_mul16', 'pce_fast_udiv16', 'pce_fast_umod16', 'pce_fast_div16')
    funcs = {name: symbol(elf, name) for name in names}
    rng = random.Random(0x51A6)
    pairs = [(0, 0), (1, 0xFFFF), (0xFFFF, 1), (0xFFFF, 0xFFFF),
             (0x8000, 2), (0x7FFF, 3), (0x1234, 0xABCD), (0xF00D, 0x0101),
             (0x8000, 0xFFFF), (0xFFFF, 0x8000)]
    dividends = (0, 1, 2, 3, 255, 256, 32767, 32768, 65534, 65535)
    divisors = (1, 2, 3, 7, 10, 33, 255, 256, 257, 32768, 65535)
    pairs += [(lhs, rhs) for lhs in dividends for rhs in divisors]
    pairs += [(rng.randrange(65536), rng.randrange(1, 65536)) for _ in range(128)]

    count = 0
    for batch_no, group in enumerate(batches(pairs, 7)):
        c = Code(); output = RESULTS
        for lhs, rhs in group:
            write_16_operands(c, lhs, rhs); c.jsr(funcs['pce_fast_mul16']); append_16_result(c, output); output += 3
            write_16_operands(c, lhs, rhs); c.jsr(funcs['pce_fast_udiv16']); append_16_result(c, output); output += 3
            write_16_operands(c, lhs, rhs); c.jsr(funcs['pce_fast_umod16']); append_16_result(c, output); output += 3
            sl = lhs if lhs < 0x8000 else lhs - 0x10000
            sr = rhs if rhs < 0x8000 else rhs - 0x10000
            if sr:
                write_16_operands(c, lhs, rhs); c.jsr(funcs['pce_fast_div16']); append_16_result(c, output); output += 3
            else:
                output += 3
        code = c.finish()
        e.write(RESULTS, bytes(output - RESULTS))
        run_raw(e, code, 2, batch=f'word batch {batch_no}')
        data = e.memory(RESULTS, output - RESULTS)
        pos = 0
        for lhs, rhs in group:
            got = struct.unpack_from('<H', data, pos)[0]; mapping = data[pos + 2]; pos += 3
            assert got == ((lhs * rhs) & 0xFFFF), ('mul16', lhs, rhs, got)
            assert mapping == MPR6_SENTINEL, ('mul16 MPR6', lhs, rhs, mapping)
            got = struct.unpack_from('<H', data, pos)[0]; mapping = data[pos + 2]; pos += 3
            assert got == (lhs // rhs if rhs else 0), ('udiv16', lhs, rhs, got)
            assert mapping == MPR6_SENTINEL, ('udiv16 MPR6', lhs, rhs, mapping)
            got = struct.unpack_from('<H', data, pos)[0]; mapping = data[pos + 2]; pos += 3
            assert got == (lhs % rhs if rhs else lhs), ('umod16', lhs, rhs, got)
            assert mapping == MPR6_SENTINEL, ('umod16 MPR6', lhs, rhs, mapping)
            sr = rhs if rhs < 0x8000 else rhs - 0x10000
            if sr:
                got = struct.unpack_from('<H', data, pos)[0]; mapping = data[pos + 2]; pos += 3
                sl = lhs if lhs < 0x8000 else lhs - 0x10000
                expected = abs(sl) // abs(sr)
                if (sl < 0) != (sr < 0): expected = -expected
                expected &= 0xFFFF
                assert got == expected, ('div16', sl, sr, got, expected)
                assert mapping == MPR6_SENTINEL, ('div16 MPR6', sl, sr, mapping)
            else:
                pos += 3
            count += 1
    return count


def test_longs(e, elf):
    names = ('pce_fast_mul32', 'pce_fast_udiv32')
    funcs = {name: symbol(elf, name) for name in names}
    rng = random.Random(0x32A6)
    mask = 0xFFFFFFFF
    pairs = [(0, 1), (1, mask), (mask, 1), (mask, mask), (mask, 0xFFFF),
             (0x80000000, 2), (0x80000000, 0xFFFFFFFF),
             (0x12345678, 0x9ABCDEF0), (0xFFFFFFFF, 0x10000),
             (0xFFFFFFFE, 0x80000001)]
    dividends = (0, 1, 255, 256, 0xFFFF, 0x10000, 0x7FFFFFFF,
                 0x80000000, 0xFFFFFFFE, 0xFFFFFFFF)
    divisors = (1, 2, 3, 90, 255, 256, 65535, 65536, 0x80000000, 0xFFFFFFFF)
    pairs += [(lhs, rhs) for lhs in dividends for rhs in divisors]
    # The optimized two-product path is selected only when lhs fits 16 bits
    # and rhs fits one byte. Cover its exact cutoff and nearby fallback.
    pairs += [(lhs, rhs) for lhs in (0, 1, 255, 256, 65535)
              for rhs in (0, 1, 2, 3, 255, 256)]
    pairs += [(rng.randrange(1 << 32), rng.randrange(1, 1 << 32)) for _ in range(128)]
    pairs = list(dict.fromkeys(pairs))
    count = 0
    for batch_no, group in enumerate(batches(pairs, 6)):
        c = Code(); output = RESULTS
        for lhs, rhs in group:
            write_32_operands(c, lhs, rhs); c.jsr(funcs['pce_fast_mul32']); append_32_result(c, output); output += 5
            if rhs:
                write_32_operands(c, lhs, rhs); c.jsr(funcs['pce_fast_udiv32']); append_32_result(c, output); output += 5
            else:
                output += 5
        code = c.finish()
        e.write(RESULTS, bytes(output - RESULTS))
        run_raw(e, code, 3, batch=f'long batch {batch_no}')
        data = e.memory(RESULTS, output - RESULTS)
        pos = 0
        for lhs, rhs in group:
            got = int.from_bytes(data[pos:pos + 4], 'little'); mapping = data[pos + 4]; pos += 5
            assert got == ((lhs * rhs) & mask), ('mul32', hex(lhs), hex(rhs), hex(got))
            assert mapping == MPR6_SENTINEL, ('mul32 MPR6', lhs, rhs, mapping)
            got = int.from_bytes(data[pos:pos + 4], 'little'); mapping = data[pos + 4]; pos += 5
            if rhs:
                assert got == lhs // rhs, ('udiv32', hex(lhs), hex(rhs), hex(got))
                assert mapping == MPR6_SENTINEL, ('udiv32 MPR6', lhs, rhs, mapping)
            count += 1
    return count


def verify(out):
    out = out.resolve(); elf = out / 'app.elf'; c = Campaign(out, sgx=True)
    with tempfile.TemporaryDirectory(prefix='sgx-fast-math-', dir=out) as base, \
         Emulator(out / 'saber_rider.cue', base, sgx=True) as e:
        boot(e, c.address)
        funcs = [symbol(elf, name) for name in
                 ('pce_fast_mul8', 'pce_fast_udiv8', 'pce_fast_umod8')]
        code = exhaustive_byte_code(*funcs)
        run_raw(e, code, 2200, batch='exhaustive byte arithmetic', expected_marker=2)

        products = read_acram(e, ACRAM_START, 65536 * 2)
        expected_products = b''.join(struct.pack('<H', a * b)
                                     for a in range(256) for b in range(256))
        assert products == expected_products, 'mul8 differs for one or more of 65,536 byte pairs'
        divisions = read_acram(e, 0x1B0000, 255 * 256 * 2)
        expected_divisions = bytes(value for a in range(256) for b in range(1, 256)
                                   for value in (a // b, a % b))
        if divisions != expected_divisions:
            index = next(i for i, (actual, expected) in enumerate(zip(divisions, expected_divisions))
                         if actual != expected)
            pair = index // 2; lhs, rhs = divmod(pair, 255); rhs += 1
            raise AssertionError(('udiv8/umod8 mismatch', lhs, rhs,
                                  'quotient' if index % 2 == 0 else 'remainder',
                                  divisions[index - index % 2:index - index % 2 + 2].hex(),
                                  expected_divisions[index - index % 2:index - index % 2 + 2].hex()))

        word_pairs = test_words(e, elf)
        long_pairs = test_longs(e, elf)
    report = {'mul8_pairs': 65536, 'udiv8_umod8_pairs': 65280,
              'word_operand_pairs': word_pairs, 'long_operand_pairs': long_pairs,
              'mpr6_restored': MPR6_SENTINEL, 'rc16_rc31_preserved': True}
    (out / 'sgx-fast-math-verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(report, flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=Path('build/sgx'))
    verify(parser.parse_args().out)
