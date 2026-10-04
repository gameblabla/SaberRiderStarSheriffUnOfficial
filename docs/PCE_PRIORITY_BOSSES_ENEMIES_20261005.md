# PCE: sprite priority, bosses, enemies, level 4 (2026-10-05)

Playtest feedback of the previous revision; everything below is native PCE code or baked assets (`src/platform/pce/`, `tools/pce/`).

## Sprite priority and flicker tracking (platform stages)

The SAT holds 64 entries and a scanline 16 width units; admission is first come, first served, so admission order is priority.
`play_draw` now admits in this order: hero, galloping herd, boss (and its lasers), enemies, enemy bullets and grenades, the hero's
bullets, the muzzle flash, then the retained foreground. When there is not enough room the hero's bullets go first, then the enemies'
bullets, and enemies last (`priority_pce.c`, bank `$7c`). The shots used to be *essential* draws ahead of the enemies; they are
optional now (a refused shot is no longer an `essential_overflow`).

Refusals are tracked per frame:

- an enemy refused on screen sets `enemy_pressure` (40 frames): the endless walker/grunt streams then stop spawning;
- an enemy bullet refused on screen sets `shot_pressure` (30 frames): no further enemy fire (a bullet nobody can see must not hurt).

The crowd therefore settles at what the hardware can show instead of flickering at its edge.

## Enemy placement and respawning

The source's trigger spawner was imitated with a three-enemy cap that skipped *placed* enemies (loops = 1: the level's own snipers,
kneelers and shield snipers) whenever the endless walker streams had filled it, and the trigger zone that wakes a placed enemy
is a screen or more behind it, so an enemy spawned that far ahead was culled at once (`x > camera + 384`). Result: the placed
kneelers and shields never appeared. Now:

- placed enemies always spawn when their zone is crossed (up to six humanoids alive); the endless streams use the room left (three
  at most, none while `enemy_pressure` is set);
- only walkers and grunts (types 1-5), which stream in from the screen edges, are culled far ahead; placed enemies stay where the
  level puts them;
- no placed or stream enemy is brought in within 160 px of a convoy's zone, and humanoids are removed while a convoy runs: the
  stampedes stay inside their sprite budget (the herd tests hold 60 Hz and have no essential overflow with the new draw order);
- the triggers carry the source's `rand_n` (the former layer byte): every respawn wait gets up to `rand_n * 0.02 s` more;
- `encounters` moved to bank `$78` (was `$6f`, full), `shots_draw_pass` / `spawn_room` live in `$7c`; the per-frame loops over
  the actor and shot pools (`actors_draw`, `world_update`, `shots_step`) walk by pointer, which saves the 21- and 13-byte index
  multiplications (about 6,000 cycles a frame in the herd scenes).

## Grenades and kneelers

The kneelers' grenades are the source sprite (`0x33269F6B`, 16x16) rendered offline in eight poses (45 degrees apart; the source
spins 0.1 rad a step) at `enemy_base + 60..67`; the pose is swapped every eight steps of flight (`Shot.t >> 3`). No software rotation.

## Shield sniper (types 30/31, stages 4 and 5)

The art is `assets/forest/sniper.png` (64x64 cells): shield up, the panel burning away (two cells), bare, and the deaths with and
without the shield (`enemy_base + 44..59`, anchor torso x 24 / feet row 52 as in `enemies.c`). Shots from the front are absorbed
(4/6/8 by difficulty) and the panel burns for 18 steps (no shots meanwhile); after that, and for a shot in the back, one hit kills.

## Bosses

- **Level 1 / 5 gunship**: the rider clone of the source (`update_boss_rider`) is baked into the hull cells: record 0 is the hull with
  the rider's level gun, record 3 the same hull with the gun at 45 degrees (used while the hero is 60+ px below, back at 48),
  so the rider costs no extra sprites and no extra scanline units. The hull was drawn 16 px high (its muzzles and hit boxes assume
  the anchor at `boss_y`); it now sits where the source puts it, which also shows the riders' heads.
- **Level 3 / 4 Hyperjumper**: its front pose is left-right symmetric (1% of pixels differ). It is baked 160 dots wide (five
  cells; the old 161-dot hull needed six and lost cells to the HUD on shared scanlines, which were the missing pieces) from the left
  half with the cockpit column from the original. The right half is the same patterns flipped (bit 15 of the piece's pattern number;
  `boss_pce.c`), so the record needs 48 instead of 96 patterns.

## Level 4

- The blue sky is a set of flat bands (`flat_sky_rows`): the wavy art made every sky character unique, and the 0.03 parallax layer left
  black holes where the sector shift outran it (the regression).
- Scroll glitches: a window filled to the brim of the 896-character cache left no room for the characters a scrolling column
  needs while the leaving column's are still held (the left edge showed other columns' tiles). The baker now keeps 32 characters
  free (`COLUMN_SLACK`); stage 4 redraws 1220 of 24,960 cells with neighbouring characters (was 648 at a zero margin).
- The cabin walls of the watchtowers (layer `ForegroundStuff`) are sprites again, in front of the hero; the plants (`ForegroundStuf2`)
  stay out.

## Continue screen

The countdown tick is the PC game's sfx 0 (`E418A101`, a new 5-bit PCM sample, trimmed to fit bank `$7f`), the confirmation sfx 8
(`A8382083`, the existing power sample). The menu blip (the hit yell) is no longer used there.
