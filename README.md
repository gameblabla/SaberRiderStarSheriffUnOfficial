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

## Controls (as in the demo)

Arrows move · **W/A** jump · **S/D** shoot · hold **Q/E** aim (8 directions) · **Enter** pause/start ·
down+jump slides · down on a platform + jump drops through. Gamepads: d-pad/stick, South/North jump,
East/West shoot, shoulders aim, Start pause.

## Debug switches (environment variables)

`SABER_START=x` spawn at level x · `SABER_MENU=n` start in front-end state n · `SABER_SHOT=file.bmp,camx,frames`
screenshot after N steps and quit · `SABER_SCRIPT="60:R,3:RJ,40:"` scripted input (L R U D J S A P) ·
`SABER_TRACE=1` per-frame player trace (+ convoy spawn / dying / stuck-enemy diagnostics) · `SABER_FUZZ=1` random input ·
`SABER_DEBUG=1` collision overlay from the start · `SABER_WINDOW=852x480` initial window size · **F1** collision overlay · **F2** free camera.

## Comparing against the original

The Linux demo runs under Xvfb (software GL). `tools/xvfb_record.py out.mp4 seconds "t:key,..."` starts an Xvfb display,
records it with ffmpeg and feeds X keysyms at the given times (`Right+` press, `Right-` release). The same script records
this port with `RUN_CMD="./build/saber_rider ../SaberRider/data" RUN_CWD=$PWD SABER_WINDOW=852x480 SABER_SCRIPT=...`.
Tile a recording with `ffmpeg -i out.mp4 -vf "fps=2,scale=213:-1,tile=6x10" -frames:v 1 tiles.png` to eyeball timing.
Original keys: arrows, A jump, S shoot, Return start/confirm, Escape quits.

## Layout

- `src/lzo1z.c` — the packs' "Pack_Crunch" codec (LZO1Z)
- `src/pack.c`, `src/gfx.c`, `src/level.c`, `src/font.c` — HEADLIST packs, cblock/sprite decoding, LEVL levels, fonts
- `src/physics.c`, `src/character.c`, `src/player.c`, `src/enemies.c`, `src/bullets.c`, `src/effects.c` — gameplay (ports of `saber_game::*`)
- `src/dialog.c`, `src/hud.c`, `src/menu.c`, `src/video.c`, `src/audio.c` — presentation
- `../docs/FORMATS.md` — file format notes; `../re/` — Ghidra decompilation used for the port

The IP belongs to Studio Pierrot / World Events Productions; this is a non-commercial preservation effort.
