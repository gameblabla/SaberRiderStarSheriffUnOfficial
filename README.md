# Saber Rider and the Star Sheriffs — 6 level game reconstruction

A C11 + SDL3 **Unofficial** re-implementation of the cancelled 2017–2019 Kickstarter game's public demo
(the "Hero Mode" level 1 with Fireball), reverse-engineered from the Linux demo executable
and its E2DM data packs. The game reads the **original** `data/*.pck` files directly; the demo's
six packs are kept unchanged in `SaberRider/data/`, the default data folder of every build
and run script (pass another `data/` folder to use your own copy).

**Updated**

I want to be extremely clear to avoid any confusion :
the game that the Kickstarter promised, was **never released**.
The last update we got from Chris Straub was from 2018 !
We only got a single demo out of it.

My point is that i used AI (Claude Opus in this instance, among others) for this project precisely
because Chris **failed to deliver the rewards and the game he promised in the first place**.

If you don't like AI and want to have the 'human-made' version,
there's the one level demo that Chris released back in 2017 that you can play instead.
I understand the irony of fully stating this but the point is, **there is no current alternative**.

Please read my full statement in the Release page to learn more, thanks.


## Build

Dependencies: SDL3, libvorbisfile, libavcodec/libswscale (FMV), CMake, a C11 compiler.

    cmake -S . -B build -G Ninja
    cmake --build build
    ./build/saber_rider SaberRider/data
    ./build/saber_rider SaberRider/data --level 2    # skip the front end: 1 the frontier town, 2 the Grand Prix, 3 Hyperjumper Pass, 4 the Red Palm Jungle

## Windows build (MinGW-w64, static)

`Makefile.win` cross-builds one static `saber_rider.exe` with no DLLs to ship. It needs `x86_64-w64-mingw32-gcc`,
the mingw-w64 static libvorbis/libogg/zlib, CMake, and curl for the first FFmpeg download.

    make -f Makefile.win -j16          # first run also builds build/win/deps -> build/win/saber_rider.exe
    make -f Makefile.win package       # build/win/SaberRider/ (exe, assets/, data/*.pck) + release/saber_rider-windows-x86_64-*.zip
    make -f Makefile.win CONSOLE=1     # build/win/saber_rider-console.exe: keeps a console (stderr / SABER_* debug output)

- **SDL3** is built as a static library from its source tarball. The makefile looks for `third_party/SDL3-*.tar.gz`,
  then `../SDL3-*.tar.gz`, or uses `SDL3_TARBALL=`.
- **FFmpeg** is a minimal static build (`FFMPEG_VERSION`, default 7.1.2; `FFMPEG_TARBALL=` for an offline copy). It
  has only the MPEG-4 and PNG decoders, the MPEG-4 parser and swscale.

Everything goes under `build/win/`, and `make -f Makefile.win distclean` removes it. The exe reads
`SaberRider/data` under the current folder when it exists, else `data/` next to itself (the package layout).
The Windows icon comes from `tools/win/` (the demo's own `icon.png`).

## Linux AppImage build

`Makefile.linux` builds the game against a private prefix so the AppImage
does not inherit the distro's SDL3/FFmpeg tree (which is where the old
`libmpg123`/codec breakage came from). It needs a C compiler, CMake,
pkg-config, curl and zlib.

    make -f Makefile.linux -j16        # first run also builds build/linux/deps -> build/linux/saber_rider

- **SDL3** is built shared from its source tarball (same lookup as
  `Makefile.win`: `third_party/SDL3-*.tar.gz`, then `../SDL3-*.tar.gz`, or
  `SDL3_TARBALL=`).
- **FFmpeg** is a minimal static build (same as Windows: MPEG-4/PNG decoders,
  MPEG-4 parser, swscale), so no external codec libraries — notably no
  `libmpg123` — are needed. **libogg/libvorbis** are static too.
- The binary's only shared third-party library is the private `libSDL3`;
  `tools/release.sh --linux` bundles it with `linuxdeploy`, which leaves the
  desktop/audio stack (X11/Wayland, GL, ALSA/PulseAudio) to the host.

Everything goes under `build/linux/`, and `make -f Makefile.linux distclean`
removes it.

## Dreamcast build

The Dreamcast target uses KallistiOS, the native PowerVR renderer, AICA ADPCM
samples, ADX music streaming, and the ZAMV5 player. It shares gameplay,
collision, levels, and menus with the SDL3 build through `src/platform/`.
It reads the demo's packs from `SaberRider/data` (override with `DATA=`).

KOS, its kos-ports and the game must share one SH4 float ABI. The build expects
`export KOS_SH4_PRECISION="-m4-single-only"` (32-bit `double`) in `environ.sh`,
with KOS and the ports (sh4zam) rebuilt after changing it.
Mixing `-m4-single` code with the `-m4-single-only` newlib breaks libm and printf
(`floorf(96)` returned 0).

```sh
source /opt/toolchains/dc/kos/environ.sh
make -f Makefile.dc -j8
make -f Makefile.dc disc      # everything
make -f Makefile.dc rebake    # textures, samples and files baked again; converted music and videos kept
make -f Makefile.dc repack    # only the new ELF
```

The playable image is `build/dc/saber_rider.cdi`. The disc builder requires
FFmpeg, Python 3 with NumPy and Pillow, the KOS `wav2adpcm`, `pvrtex`,
`scramble`, and `makeip` utilities, the DCMV converter vendored in
`third_party/dreamcast-fmv` (its packers are compiled on first use: gcc,
liblz4, libzstd), `mkisofs`, and `cdi4dc`.

Nothing is decoded on the console (apart from an LZ4 unpack of a few blocks). The builder bakes everything into three
extra packs in `/cd/data`, in the same HEADLIST format. Every block is 32-byte
aligned and padded, so it takes one read and goes on by DMA:

- `tex.pck`: every texture. `tools/dc/texprep.c` runs the game's own `gfx.c`
  over the packs' sprites, tile banks and fonts. `tools/dc/texbake.py` then
  bakes those and our PNGs into twiddled power-of-two pages. Up to 16 colours
  use a 4-bit palette and up to 256 an 8-bit one, both exact. An image with
  more colours keeps its 16-bit format (RGB565, ARGB1555, ARGB4444) unless
  pvrtex's VQ or 256-colour quantisation measures no worse against the source.
  Tile banks are re-laid in the sheet width that needs the least memory. At run
  time the colours go into palette RAM (ARGB8888), shared between textures. A
  texture is expanded to 16 bits instead if it would cost at most 8 KB that way,
  if palette RAM has no room, or if it saves less than 768 bytes of VRAM per
  new entry. The 1024 entries go to the backgrounds and sprite sheets that save
  the most. Stage 2's atlas and sky (`LZ4_TEX` in `build_disc.py`) are stored
  LZ4-compressed (224 + 108 KB down to 51 + 12 KB) and decoded on load.
- `snd.pck`: the packs' sfx and our WAVs as AICA ADPCM (`wav2adpcm`), padded
  to 32 bytes.
- `files.pck`: our text and level files, plus the RGBA of the three images whose
  pixels the game reads, LZ4-compressed. Of `mode7.png` only the floor strip is
  kept (5 KB instead of 860 KB). Reading the whole image took 7 s of stage 2's
  load in Flycast.

Music goes to ADX files, and the pack videos and power-attack clips to ZAMV5
files. Those stream. Do not copy `video.pck` to the disc: the runtime opens the
converted files in `/cd/video` instead. Stages load what they use when they
start: the level's graphics, and every enemy, shot and effect its triggers can
spawn. The sfx table and the stage's own sounds load too, so nothing is read
from the disc mid-level. A music change (the boss's arrival, the mission jingle) is handed to a thread
of its own, which owns one persistent KOS PCM stream and opens the next track
while the game runs on. ADX tracks up to 512 KiB (including the mission jingle
and victory screen music) are loaded completely before playback; larger tracks
have two aligned 64 KiB buffers of compressed read-ahead. The final PCM tail is
padded with silence and drained before stopping. Repeating effects such as turbo
are decoded once to PCM so their ADPCM predictor cannot drift across repeats.
See [the audio fix notes](docs/DREAMCAST_AUDIO_FIX.md) for the source review.
`SABER_READLOG=1` logs every pack read, and
`SABER_VRAMLOG=1` logs every texture with the VRAM and palette use.

A CD-R is read at constant linear velocity. The image is padded to 650 MiB
(`--pad-to`, 0 turns it off) with a dummy file sorted first, so the game's
files sit on the outer part of the disc.

Controls: D-pad or stick moves, A jumps, B/X shoots, Y uses the power attack,
triggers aim, and Start pauses. A+B+X+Y+Start resets to the console menu.
For developer runs, put `NAME=value` lines such as `SABER_STAGE=3` in
`build/dc/stage/saber.env`, then rebuild the ISO/CDI from that stage directory.

OPTIONS > SCREEN also offers 320x240 on every cable: KOS's `DM_320x240`,
pixel- and line-doubled on VGA and 240p on RGB, S-video or composite. It is
always 4:3 at 1x, and RATIO is held at 4:3 there.

With a VGA cable, OPTIONS > SCREEN also offers `VGA 832x480`, this mode requires an OSSC or DCHMI for it to function. 

## Release packages

    tools/release.sh [--linux] [--dc] [--full-disc] [path/to/SaberRider/data]   # packs default to SaberRider/data

The script writes release packages to `release/`. Linux packaging builds its
own SDL3 plus a minimal static FFmpeg/Vorbis stack (`make -f Makefile.linux`,
needs an SDL3 source tarball in `third_party/` or next to the repo) and
bundles the AppDir with `linuxdeploy`, which is downloaded on first use;
set `LINUXDEPLOY=/path/to/linuxdeploy` to use a specific executable. If it
cannot download its runtime, set `APPIMAGE_RUNTIME=/path/to/runtime-x86_64`.

- `saber_rider-linux-x86_64-<date>-<commit>.AppImage` is a self-contained Linux
  app with the private SDL3 build, statically linked video/audio decoders,
  icon, assets and all six demo packs, including `video.pck`. glibc,
  X11/Wayland, GL and the sound server come from the user's system. Run it
  directly; `--level N` starts at stage N.
- `saber_rider-dreamcast-<date>-<commit>.zip` has `saber_rider.cdi`, the
  256×256 front cover, and a README.

The Dreamcast part sources `$KOS_ENV` (default
`/opt/toolchains/dc/kos/environ.sh`). It repacks `build/dc` when that disc was
already built; otherwise, or with `--full-disc`, it builds the whole disc. It
refuses to run while `build/dc/stage/saber.env` exists, because that file holds
debug switches.

## Debug switches (environment variables)

`SABER_START=x` spawn at level x · `SABER_MENU=n` start in front-end state n · `SABER_SHOT=file.bmp,camx,steps`
screenshot after N fixed steps and quit · `SABER_SCRIPT="60:R,3:RJ,40:"` scripted input (L R U D J S A P, X power) ·
`SABER_TRACE=1` per-frame player trace (+ spawn triggers at start, convoy spawn / dying / stuck-enemy diagnostics, every humanoid once a second, boss state every 10 frames; `=2` also prints humanoids within 40 px of either screen edge every frame) · `SABER_FUZZ=1` random input ·
`SABER_HERO=n` hero 0..3 for a direct level start · `SABER_LIVES=n` starting lives · `SABER_KILL=n` kill the player at step n · `SABER_BOSSHP=n` the horse boss's / Hyperjumper's HP · `SABER_BORED=s` seconds of idling before the bored animation (April / Saber) · `SABER_DEBUG=1` collision overlay from the start · `SABER_WINDOW=852x480` initial window size · **F1** collision overlay · **F2** free camera.
`SABER_PERF=1` a line a second on stderr (the serial log on the Dreamcast): the camera, live enemies, update and draw
time (average / worst), the worst frame, frames that ran no step or two and more, the Dreamcast's draw count, and pack reads
(a read in the middle of a level is a stall).

The IP belongs to Studio Pierrot / World Events Productions; this is a non-commercial preservation effort.
