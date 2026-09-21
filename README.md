# Saber Rider and the Star Sheriffs — demo reconstruction

A C11 + SDL3 re-implementation of the cancelled 2017–2019 Kickstarter game's public demo
(the "Hero Mode" level 1 with Fireball), reverse-engineered from the Linux demo executable
and its E2DM data packs. The game reads the **original** `data/*.pck` files directly — nothing
from the demo is redistributed here. You need your own copy of the demo (Windows/Linux/macOS
builds share the same packs).

## Build

Dependencies: SDL3, libvorbisfile, libavcodec/libswscale (FMV), CMake, a C11 compiler.

    cmake -S . -B build -G Ninja
    cmake --build build
    ./build/saber_rider /path/to/SaberRider/data

## Playable heroes

Fireball (the demo's hero, from the packs) and **April** (our reconstruction: `assets/april.png`, built from the
archived sprite sheet and tweet clips by `../heroes/`, loaded by `src/heroes.c` on top of her shipped CRHC
`79260A58`; the level-1 dialogs are rewritten for her; her "OK!" on the character select and her jump / hurt / death /
fall grunts in `assets/voice/` replace Fireball's table samples on the same events and were generated with OmniVoice,
cloned from her cartoon voice, by `../heroes/voice/generate.py`).
Her idle sway (6 frames, also her alert pose), run cycle (7 frames, split at the hip into legs and torso overlay so
level shots on the move keep the clip pixels while the up / down diagonals draw the sheet's aim torsos over the clip
legs) and the jumping jacks + wave she plays after 10 s of standing still are the artist's own clip frames; the rest
of the sheet is recovered from the archived 700 px sheet by `../heroes/learn_inverse.py` (a U-Net trained on the
game's own sprites under the sheet's exact bicubic downscale, then a joint palette solve against the sheet).
Saber Rider and Colt stay "not available". Assets are looked up in
`$SABER_ASSETS`, `./assets` and next to the executable.

## Stage 2 — "The All Galaxy Grand Prix" (Mode-7)

After the frontier town's MISSION ACCOMPLISHED the game continues with a full-screen SNES-style Mode-7 stage built
on anime episode 28: Fireball races the New Borderland circuit against the retiring champion Marco Firenza and the
"Black Hornets" team - Outriders in disguise whose real target is the Cavalry Command Nerve Center in Dome City
(the fan "Claudia" who lures Fireball into a trap and April's rescue open the stage as a dialog scene). One lap of
racing with guns (the Hornets fire back and drop mines), then the Hornets break away and the stage turns into a
desert pursuit toward Dome City with April riding alongside on Nova and Outrider tanks in the way, and ends with the
Hornet leader shelling the dome on the city plaza.

`src/mode7.c` draws the floor per scanline into a streaming texture (affine floor, fog, wrapping 256x256 tile
world), the level-1 sky / mountain layers from the packs as the horizon (`level_draw_layer_strip`), and distance-
sorted scaled billboards. Art: `assets/mode7.png` + `mode7.txt`, built by `tools/build_mode7_assets.py` from the
tweet clips (the buggy's 6 rear-view frames, April on Nova, Fireball's rear-view hop, the Outrider tank lifted from
the 320x240 Mode-7 mockup), the level-1 cacti / rock spires, recolours of the buggy for the field, and drawn floor
tiles, Dome City, shots, mines and explosions. The stage is Fireball's story whichever hero was picked.

Controls: left/right steer · jump button or up accelerate · shoot button fire · aim button turbo (meter) ·
down brake · Enter pause. Off the asphalt the car is slow; kerbs rattle; rocks and cacti are solid.
Debug: `SABER_STAGE=2` starts there, `SABER_M7PHASE=1|2|3` skips to the race / pursuit / boss (`SABER_M7LAP=1`
starts on lap 2 so the breakaway fires after 5 s, `SABER_M7BOSSHP=n` sets the leader's HP).

## Controls (as in the demo)

Arrows move · **W/A** jump · **S/D** shoot · hold **Q/E** aim (8 directions) · **Enter** pause/start ·
down+jump slides · down on a platform + jump drops through. Gamepads: d-pad/stick, South/North jump,
East/West shoot, shoulders aim, Start pause.

## Debug switches (environment variables)

`SABER_START=x` spawn at level x · `SABER_MENU=n` start in front-end state n · `SABER_SHOT=file.bmp,camx,steps`
screenshot after N fixed steps and quit · `SABER_SCRIPT="60:R,3:RJ,40:"` scripted input (L R U D J S A P) ·
`SABER_TRACE=1` per-frame player trace (+ spawn triggers at start, convoy spawn / dying / stuck-enemy diagnostics, every humanoid once a second, boss state every 10 frames; `=2` also prints humanoids within 40 px of either screen edge every frame) · `SABER_FUZZ=1` random input ·
`SABER_HERO=n` hero 0..3 for a direct level start · `SABER_KILL=n` kill the player at step n · `SABER_BORED=s` seconds of idling before the bored animation (April) · `SABER_DEBUG=1` collision overlay from the start · `SABER_WINDOW=852x480` initial window size · **F1** collision overlay · **F2** free camera.

## Comparing against the original

The Linux demo runs under Xvfb (software GL). `tools/xvfb_record.py out.mp4 seconds "t:key,..."` starts an Xvfb display,
records it with ffmpeg and feeds X keysyms at the given times (`Right+` press, `Right-` release). The same script records
this port with `RUN_CMD="./build/saber_rider ../SaberRider/data" RUN_CWD=$PWD SABER_WINDOW=852x480 SABER_SCRIPT=...`.
`AUDIO=1` adds the game's sound to the mp4 (played into a PulseAudio/PipeWire null sink; the original's static SDL2
honours `PULSE_SINK`, the port's stream is moved there with `pactl`), which is how music fades, stops and sample timing
were compared (RMS envelope per 50 ms, or cross-correlation against a decoded sfx).
`tools/pose_sheet.py out.png [hero] [x]` runs the game headless through every player pose and tiles crops of the hero (sprite review).
Tile a recording with `ffmpeg -i out.mp4 -vf "fps=2,scale=213:-1,tile=6x10" -frames:v 1 tiles.png` to eyeball timing.
Original keys: arrows, A jump, S shoot, Return start/confirm, Escape quits.

## Layout

- `src/lzo1z.c` — the packs' "Pack_Crunch" codec (LZO1Z)
- `src/pack.c`, `src/gfx.c`, `src/level.c`, `src/font.c` — HEADLIST packs, cblock/sprite decoding, LEVL levels, fonts
- `src/physics.c`, `src/character.c`, `src/player.c`, `src/enemies.c`, `src/bullets.c`, `src/effects.c` — gameplay (ports of `saber_game::*`)
- `src/dialog.c`, `src/hud.c`, `src/menu.c`, `src/video.c`, `src/audio.c` — presentation
- `src/mode7.c` — stage 2, the Mode-7 Grand Prix (our own design, see above)
- `src/audio.c` mixes deliberately *unlike* the original: the demo's mixer (`FUN_00563900`) sums the music at vol/256
  and every sfx voice at unity into 16-bit and hard-clips at ±0x7fbc, and the material is mastered hot (most sfx and
  the music tracks peak at 0 dBFS or above), so a voice line over a gunshot clips. The port decodes the music in float,
  gives the sfx / voice / music buses headroom (`GAIN_*`) and runs a peak limiter in SDL3's post-mix callback.
- `../docs/FORMATS.md` — file format notes; `../re/` — Ghidra decompilation used for the port

The IP belongs to Studio Pierrot / World Events Productions; this is a non-commercial preservation effort.
