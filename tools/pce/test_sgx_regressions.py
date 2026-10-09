#!/usr/bin/env python3
"""Verify the native SGX gunship hit frame keeps its hull and palettes coherent."""
import argparse
import json
import struct
from collections import Counter
from pathlib import Path
from PIL import Image

from emulator import Emulator, boot, symbol
from test_campaign import Campaign
from test_sgx_rendering import Rendering


def read16(e, addr):
    return int.from_bytes(e.memory(addr, 2), 'little')


def sat_rows(e, vdc):
    raw = bytes.fromhex(e.call('asread', f'sat{vdc}', 0, 512)['hex'])
    return list(struct.iter_unpack('<4H', raw))


def palette(e, index):
    return bytes.fromhex(e.call('asread', 'pram', index * 32, 32)['hex'])


def expected_hull(scene, blob, e, camera, bx, by, level, flipped, verify_pattern_bytes=True):
    record = struct.unpack_from('<IIIHB', blob, scene['boss_big_offset'] + 15 * level)
    _palette_addr, parts_addr, patterns_addr, pattern_bytes, count = record
    parts = [struct.unpack_from('<hhh', blob, parts_addr + 6 * i) for i in range(count)]
    expected = []
    for dx, dy, piece in parts:
        if flipped:
            dx = -dx - 32
        sx, sy = bx - camera + dx, by + dy
        if sx <= -32 or sx >= 256 or sy <= -32 or sy >= 224:
            continue
        pattern = (0x5800 >> 5) + (piece & 0x7fff) * 2
        expected.append((sy + 64, sx + 32, pattern))
    if verify_pattern_bytes:
        vram = bytes.fromhex(e.call('asread', 'vram1', 0x5800 * 2, pattern_bytes)['hex'])
        assert vram == blob[patterns_addr:patterns_addr + pattern_bytes], (
            'VDC1 hull patterns differ from stage archive')
    actual = [row for row in sat_rows(e, 1) if row[0] and row[3] & 15 == 14]
    if not expected:
        return {'level': level, 'expected_visible_parts': 0,
                'actual_palette14_parts': len(actual), 'offscreen': True}
    actual_keys = [(row[0], row[1], row[2]) for row in actual]
    common_offset = None
    # `pce_presented` advances in VBlank before the native loop completes its
    # next tick. The published SAT can therefore be one camera/bob step behind
    # the live globals. Require one shared, bounded translation for every
    # source part while keeping each source pattern and part coordinate paired.
    def matches_offset(dx, dy):
        available = Counter(actual_keys)
        for y, x, pattern in expected:
            key = (y + dy, x + dx, pattern)
            if not available[key]:
                return False
            available[key] -= 1
        return True

    for dy in range(-2, 3):
        for dx in range(-8, 9, 2):
            if matches_offset(dx, dy):
                common_offset = (dx, dy)
                break
        if common_offset is not None:
            break
    assert common_offset is not None, ('visible hull parts missing or incoherent in VDC1 SAT',
                                       level, expected, actual)
    # Mednafen screenshots include a 12x11 outer border around the 256x224
    # image. Inflate the expected hull box for the one-publication pose skew.
    box = (max(0, min(x for _y, x, _p in expected) - 32 + 12 - 8),
           max(0, min(y for y, _x, _p in expected) - 64 + 11 - 8),
           min(280, max(x for _y, x, _p in expected) - 32 + 32 + 12 + 8),
           min(242, max(y for y, _x, _p in expected) - 64 + 32 + 11 + 8))
    return {'level': level, 'expected_visible_parts': len(expected),
            'actual_palette14_parts': len(actual), 'palette14_vdc1': True,
            'published_pose_offset_pixels': common_offset,
            'screenshot_hull_bbox': box}


def white_hull_pixels(path, box):
    image = Image.open(path).convert('RGB')
    x0, y0, x1, y1 = box
    crop = image.crop((x0, y0, x1, y1))
    raw = crop.tobytes()
    white = bytes((252, 252, 252))
    return sum(raw[i:i + 3] == white for i in range(0, len(raw), 3))


def run(out):
    out = out.resolve()
    evidence = out / 'sgx-regression-evidence'
    evidence.mkdir(parents=True, exist_ok=True)
    c = Campaign(out, sgx=True)
    elf = out / 'app.elf'
    names = ('boss_phase', 'boss_flash', 'boss_x', 'boss_y', 'boss_hold', 'boss_dir',
             'hull_level', 'hull_ready', 'shots', 'player', 'camera', 'safe_timer',
             'pce_presented', 'vce_q_head', 'vce_q_tail')
    sym = {name: symbol(elf, name) for name in names}
    scene = json.loads((out / 'manifest.json').read_text())['scenes'][0]
    blob = (out / 's1.bin').read_bytes()
    report = {}

    with Emulator(out / 'saber_rider.cue', evidence / 'emulator', sgx=True) as e:
        boot(e, c.address)
        e.run(120)
        c.seed(e, 'dialogs_done', 255, 1)
        c.seed(e, 'safe_timer', 250, 1)
        c.move(e, 9800)
        c.until(e, lambda: c.state(e)['boss_kind'] == 1, limit=900)
        c.dialogs(e)
        c.until(e, lambda: e.memory(sym['boss_phase'], 1) == b'\x02', limit=6000, step=10)

        # Hold the native stage-1 gunship in its vulnerable sweep while placing
        # it fully onscreen. A centered hero shot below exercises real collision,
        # HP decrement, hull palette change, and SAT publication on the CPU.
        camera = c.metrics(e)['camera_x']
        bx, by = camera + 128, 48
        c.seed(e, 'camera', camera)
        c.seed(e, 'boss_x', bx)
        c.seed(e, 'boss_y', by)
        c.seed(e, 'boss_phase', 2, 1)
        c.seed(e, 'boss_hold', 3)
        c.seed(e, 'boss_dir', 0, 1)
        c.seed(e, 'boss_flash', 0, 1)
        c.seed(e, 'boss_time', 0)
        c.seed(e, 'safe_timer', 250, 1)
        c.field(e, 'boss_hp', 10)
        player = bytearray(e.memory(sym['player'], 16))
        struct.pack_into('<2h', player, 0, min(camera + 184, 9900), 120)
        e.write(sym['player'], player)
        e.write(sym['shots'], bytes(208))
        c.seed(e, 'hull_level', 3, 1)
        c.seed(e, 'hull_ready', 0, 1)
        e.input(0)
        e.run(4)
        assert e.memory(sym['hull_ready'], 1) == b'\x01', 'native boss hull did not load'
        fg_palette_before = palette(e, 28)
        hp_before = c.state(e)['boss_hp']

        hit = None
        for _ in range(6):
            bx, by = (read16(e, sym['boss_x']), read16(e, sym['boss_y']))
            e.write(sym['shots'], struct.pack('<4h5B', bx, by, 0, 0, 1, 0, 0, 0, 0) + bytes(195))
            e.run(1)
            state = c.state(e)
            flash = e.memory(sym['boss_flash'], 1)[0]
            if state['boss_hp'] < hp_before and flash:
                hit = {'hp': state['boss_hp'], 'flash': flash,
                       'camera': c.metrics(e)['camera_x'],
                       'boss_x': read16(e, sym['boss_x']),
                       'boss_y': read16(e, sym['boss_y']),
                       'hull_level': e.memory(sym['hull_level'], 1)[0],
                       'boss_dir': e.memory(sym['boss_dir'], 1)[0],
                       'presented': read16(e, sym['pce_presented'])}
                break
        assert hit, ('native boss collision did not produce a visible hit flash', c.state(e),
                     e.memory(sym['boss_flash'], 1).hex())

        # A VBlank applies the queued VCE palette after drawing; the emulator
        # screenshot therefore reflects the following output frame, even when
        # the SAT-generation counter does not change for a palette-only update.
        white = palette(e, 30)
        for _ in range(120):
            if white == b'\xff\x01' * 16:
                break
            if not e.memory(sym['boss_flash'], 1)[0]:
                break
            e.run(1)
            white = palette(e, 30)
        hit['palette30_white_observed'] = white == b'\xff\x01' * 16
        hit['palette30_at_hit_check'] = white.hex()
        assert hit['palette30_white_observed'], ('boss hit palette 30 is not white', white.hex(), hit,
            e.memory(sym['vce_q_head'], 1).hex(), e.memory(sym['vce_q_tail'], 1).hex())
        hull = expected_hull(scene, blob, e, hit['camera'], hit['boss_x'], hit['boss_y'],
                             hit['hull_level'], bool(hit['boss_dir']))
        hit['hull'] = hull
        immediate = evidence / 'boss-hit-flash-palette-write.png'
        e.screenshot(immediate)
        hit['pixels_white_immediately_after_palette_write'] = white_hull_pixels(
            immediate, hull.get('screenshot_hull_bbox', (0, 0, 280, 242)))
        white_hull = False
        samples = []
        capture = evidence / 'boss-hit-flash.png'
        for frame in range(120):
            pram_before = palette(e, 30)
            flash_before = e.memory(sym['boss_flash'], 1)[0]
            if pram_before != b'\xff\x01' * 16:
                break
            presented_before = read16(e, sym['pce_presented'])
            e.run(1)
            presented_after = read16(e, sym['pce_presented'])
            e.screenshot(capture)
            pixels = white_hull_pixels(capture, hull.get('screenshot_hull_bbox', (0, 0, 280, 242)))
            samples.append({'frame': frame + 1, 'flash_before_frame': flash_before,
                            'pram_white_before_frame': True,
                            'presentation_delta': (presented_after - presented_before) & 0xffff,
                            'white_hull_pixels': pixels})
            if pixels >= 256:
                white_hull = True
                break
            if not e.memory(sym['boss_flash'], 1)[0] and palette(e, 30) != b'\xff\x01' * 16:
                break
        hit['white_hull_presentation_observed'] = white_hull
        hit['white_hull_screenshot_samples'] = samples
        hit['white_hull_pixel_threshold'] = 256
        hit['palette30_after_capture_wait'] = palette(e, 30).hex()
        if not white_hull:
            hit['failure'] = ('no captured emulator frame showed the VDC1 hull white while palette 30 was white; '
                              'the strict pixel-presentation check failed')
        assert palette(e, 28) == fg_palette_before, 'foreground palette 12 changed during boss hit flash'
        hit['foreground_palette_28_unchanged'] = True
        report['boss_hit'] = hit

        # A hit flash lasts four game steps. Advance until the real palette
        # restore path runs, then compare with the archive's hull colors.
        for _ in range(12):
            if e.memory(sym['boss_flash'], 1) == b'\0':
                break
            e.run(1)
        assert e.memory(sym['boss_flash'], 1) == b'\0', 'boss flash did not expire'
        record = struct.unpack_from('<IIIHB', blob,
                                    scene['boss_big_offset'] + 15 * e.memory(sym['hull_level'], 1)[0])
        restored = blob[record[0]:record[0] + 32]
        # Flash countdown and palette publication occur at different points
        # within the game step. Wait for the native draw after expiry.
        for _ in range(12):
            if palette(e,30)==restored:break
            e.run(1)
        assert palette(e, 30) == restored, ('boss palette 30 did not restore from archive',
                                              palette(e, 30).hex(), restored.hex())
        assert palette(e, 28) == fg_palette_before, 'foreground palette 12 changed after boss flash restore'
        e.screenshot(evidence / 'boss-after-flash.png')
        report['boss_restore'] = {'flash': 0, 'palette30_restored': True,
                                  'foreground_palette28_unchanged': True}

        # Sustain fire in the actual locked stage-1 boss arena. The convoy
        # fixture separately supplies dense foreground pressure; here every
        # foreground part that exists must remain represented while we check
        # the native hull and both projectile classes over sixty publications.
        render = Rendering(out)
        camera = c.metrics(e)['camera_x']
        bx = camera + 128
        c.seed(e, 'boss_x', bx)
        c.seed(e, 'boss_y', 48)
        c.seed(e, 'boss_phase', 2, 1)
        c.seed(e, 'boss_hold', 3)
        c.seed(e, 'boss_dir', 0, 1)
        c.seed(e, 'boss_flash', 0, 1)
        c.seed(e, 'boss_time', 0)
        c.field(e, 'boss_hp', 30)
        c.seed(e, 'safe_timer', 250, 1)
        c.seed(e, 'hull_level', 3, 1)
        c.seed(e, 'hull_ready', 0, 1)
        e.write(sym['player'], struct.pack('<4h4B', camera + 100, 120, 0, 0, 0, 0, 4, 4))
        e.write(sym['shots'], bytes(208))
        e.input(0)
        e.run(8)
        palette28 = palette(e, 28)
        initial_fg = render.foreground(e, allow_camera_lag=True)

        e.input(1)
        presented = read16(e, sym['pce_presented'])
        samples = 0
        hero_bullet_samples = boss_bullet_samples = foreground_parts = 0
        hero_bullet_missing_candidates = boss_bullet_missing_candidates = 0
        hero_candidates = boss_candidates = 0
        foreground_nonempty_samples = onscreen_hull_samples = 0
        foreground_camera_shifts = set()
        foreground_snapshot_generations = set()
        emulator_frames = 0
        camera_min = camera_max = camera
        prior_shots = bytes(208)
        while samples < 60 and emulator_frames < 600:
            before = presented
            # The native stage boss camera continues its prescribed glide.
            # Keep the player close enough to exercise own-fire admission and
            # calculate checks from each presented camera generation.
            camera_now = c.metrics(e)['camera_x']
            player = bytearray(e.memory(sym['player'], 16))
            struct.pack_into('<h', player, 0, camera_now + 100)
            e.write(sym['player'], player)
            previous_shots = e.memory(sym['shots'], 208)
            # pce_presented advances in VBlank before the next gameplay tick
            # finishes; retain the exact world pose whose SAT is published by
            # this frame rather than reading the one-pixel-advanced CPU state.
            sample_camera = camera_now
            sample_bx = read16(e, sym['boss_x'])
            sample_by = read16(e, sym['boss_y'])
            sample_level = e.memory(sym['hull_level'], 1)[0]
            sample_dir = bool(e.memory(sym['boss_dir'], 1)[0])
            previous_foreground = render.foreground_snapshot(e)
            e.run(1)
            emulator_frames += 1
            presented = read16(e, sym['pce_presented'])
            delta = (presented - before) & 0xffff
            if not delta:
                continue
            assert delta == 1, ('skipped presented generation during sustained-fire sample', delta)
            samples += 1
            state_metrics = c.metrics(e)
            camera = sample_camera
            camera_min = min(camera_min, camera);camera_max = max(camera_max, camera)
            hull = expected_hull(scene, blob, e, camera, sample_bx, sample_by, sample_level,
                                 sample_dir,
                                 verify_pattern_bytes=samples in (1, 60))
            onscreen_hull_samples += int(hull['expected_visible_parts'] > 0)
            current_foreground = render.foreground_snapshot(e)
            foreground_match = None
            foreground_error = None
            for generation, snapshot in (('prior', previous_foreground),
                                         ('current', current_foreground)):
                try:
                    retained = render.validate_foreground(e, snapshot, allow_camera_lag=True)
                    foreground_match = generation
                    break
                except AssertionError as error:
                    foreground_error = error
            assert foreground_match is not None, ('foreground absent from prior/current publication',
                                                   samples, foreground_error)
            foreground_parts += retained
            foreground_nonempty_samples += int(retained > 0)
            foreground_camera_shifts.add(render.last_fg_camera_shift)
            foreground_snapshot_generations.add(foreground_match)
            assert palette(e, 28) == palette28, ('foreground palette 12 changed while firing', samples)

            rawshots = e.memory(sym['shots'], 208)
            hero_candidate_count = 0
            boss_candidate_counts = {}
            for k in range(16):
                prior = struct.unpack_from('<4h5B', prior_shots, k * 13)
                old = struct.unpack_from('<4h5B', previous_shots, k * 13)
                new = struct.unpack_from('<4h5B', rawshots, k * 13)
                ox, oy, ovx, ovy, old_active, old_enemy, *_ = old
                x, y, vx, vy, active, enemy, *_ = new
                if not old_active or not active or old_enemy != enemy:
                    continue
                # VBlank can publish the SAT before the CPU completes the
                # next tick and spawns a shot. That newborn is already in
                # previous_shots at the following debugger frame boundary,
                # although it has never belonged to the published SAT. Require
                # continuity from the preceding publication as well, and
                # reject a pool slot reused for a different trajectory.
                if not prior[4] or prior[5] != enemy or prior[2:4] != old[2:4]:
                    continue
                # Ignore a bullet unless both ends of this publication step
                # keep its 16x16 image fully onscreen. A newborn or leaving
                # image may naturally be one generation ahead of the SAT.
                positions = ((ox, oy), (x, y))
                if not all(0 <= px - camera <= 240 and 16 <= py <= 224 for px, py in positions):
                    continue
                if enemy == 0:
                    hero_candidate_count += 1
                    hero_candidates += 1
                elif enemy == 3:
                    sprite_id = 45 + (1 if vy and vx else 2 if vy else 0)
                    boss_candidate_counts[sprite_id] = boss_candidate_counts.get(sprite_id, 0) + 1
                    boss_candidates += 1
            hero_entries = render.sprite_entries(e, 36) if hero_candidate_count else []
            hero_missing = max(0, hero_candidate_count - len(hero_entries))
            assert not hero_missing, ('persistent onscreen hero bullets lack per-VDC SAT entries',
                                      samples, hero_candidate_count, hero_entries)
            hero_bullet_samples += int(hero_candidate_count > 0 and len(hero_entries) >= hero_candidate_count)
            hero_bullet_missing_candidates += hero_missing
            boss_sample_count = 0
            for sprite_id, candidate_count in boss_candidate_counts.items():
                entries = render.sprite_entries(e, sprite_id)
                missing = max(0, candidate_count - len(entries))
                assert not missing, ('persistent onscreen boss projectiles lack per-VDC SAT entries',
                                     samples, sprite_id, candidate_count, entries)
                boss_sample_count += candidate_count
                boss_bullet_missing_candidates += missing
            boss_bullet_samples += int(boss_sample_count > 0)
            prior_shots = previous_shots
            assert e.memory(sym['boss_flash'], 1)[0] < 5
        e.input(0)
        assert samples == 60, ('sustained-fire presentation count', samples, emulator_frames)
        assert onscreen_hull_samples > 10, ('native boss was offscreen for the sustained fixture', onscreen_hull_samples)
        assert hero_candidates > 10 and hero_bullet_samples > 10, (
            'hero bullet continuity was vacuous', hero_candidates, hero_bullet_samples)
        assert boss_candidates > 10 and boss_bullet_samples > 10, (
            'boss bullet continuity was vacuous', boss_candidates, boss_bullet_samples)
        report['sustained_fire'] = {
            'published_presentations': samples,
            'camera_x_min': camera_min,
            'camera_x_max': camera_max,
            'onscreen_hull_presentations': onscreen_hull_samples,
            'foreground_parts_checked': foreground_parts,
            'foreground_nonempty_presentations': foreground_nonempty_samples,
            'foreground_parts_at_setup': initial_fg,
            'foreground_common_publication_shifts': sorted(foreground_camera_shifts),
            'foreground_sat_matched_snapshot': sorted(foreground_snapshot_generations),
            'every_visible_retained_foreground_part_matched_when_present': True,
            'foreground_palette28_stable': True,
            'hero_projectile_presentations': hero_bullet_samples,
            'boss_projectile_presentations': boss_bullet_samples,
            'persistent_onscreen_hero_bullet_candidates': hero_candidates,
            'persistent_onscreen_boss_bullet_candidates': boss_candidates,
            'hero_live_bullet_without_sat_match': hero_bullet_missing_candidates,
            'boss_live_bullet_without_sat_match': boss_bullet_missing_candidates,
            'hull_patterns_checked_at': [1, 60],
            'all_onscreen_hull_parts_present': True,
            'all_visible_bullets_use_valid_per_vdc_pattern_words': True,
        }
        e.screenshot(evidence / 'boss-sustained-fire.png')

    (out / 'sgx-regression-verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    if not report['boss_hit']['white_hull_presentation_observed']:
        raise AssertionError(report['boss_hit']['failure'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, default=Path('build/sgx'))
    run(parser.parse_args().out)
