#!/usr/bin/env bash
# Package a release in release/
#   saber_rider-linux-x86_64-<version>.AppImage  the SDL3 build, bundled libraries, assets/ and data/*.pck
#   saber_rider-windows-x86_64-<version>.zip  the MinGW-w64 static build (no DLLs to ship), assets/ and the demo's data/*.pck
#   saber_rider-dreamcast-<version>.zip     the self-booting CDI (the packs are on the disc)
#   saber_rider-saturn-<version>.zip        the CD image (a single cue/bin, like a retail disc; the packs are baked onto it)
#   saber_rider-pce-cdrom2-<version>.zip    the PC Engine Arcade CD-ROM² image (cue/ISO plus the 57 CD-DA music tracks; needs a Super CD-ROM² drive on a v3.0 BIOS plus an Arcade Card HuCard)
#
#   tools/release.sh [--linux] [--win] [--dc] [--sat] [--pce] [--full-disc] [DATA_DIR]
#     --linux / --win / --dc / --sat / --pce   only those packages (default: all)
#     --full-disc      rebuild the whole Dreamcast disc (music and FMV conversion too) instead of re-baking build/dc/stage
#     DATA_DIR         the demo's data/ folder with the .pck packs (default SaberRider/data, the copy in this repo)
# The Dreamcast part sources $KOS_ENV (default /opt/toolchains/dc/kos/environ.sh).
# The Saturn part sources $YAUL_ENV (default ~/.yaul.env) and always rebuilds build/saturn from scratch.
# The PC Engine part builds with the LLVM-MOS SDK in $MOS (default ../PCE/llvm-mos8) and needs no data packs.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

want_linux=0 want_win=0 want_dc=0 want_sat=0 want_pce=0 full_disc=0 DATA=""
for a in "$@"; do
    case "$a" in
        --linux) want_linux=1 ;;
        --win|--windows) want_win=1 ;;
        --dc) want_dc=1 ;;
        --sat|--saturn) want_sat=1 ;;
        --pce|--pcengine|--turbografx) want_pce=1 ;;
        --full-disc) full_disc=1 ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        -*) echo "unknown option $a" >&2; exit 2 ;;
        *) DATA="$a" ;;
    esac
done
if [ $want_linux = 0 ] && [ $want_win = 0 ] && [ $want_dc = 0 ] && [ $want_sat = 0 ] && [ $want_pce = 0 ]; then
    want_linux=1 want_win=1 want_dc=1 want_sat=1 want_pce=1
fi
DATA="$(cd "${DATA:-$ROOT/SaberRider/data}" && pwd)"
PACKS=(pack.pck common.pck levels.pck menu.pck level1.pck video.pck)
# The PC Engine disc bakes its own archives, so it needs no packs.
if [ $want_linux = 1 ] || [ $want_win = 1 ] || [ $want_dc = 1 ] || [ $want_sat = 1 ]; then
    for p in "${PACKS[@]}"; do [ -f "$DATA/$p" ] || { echo "missing $DATA/$p (pass the demo's data/ folder)" >&2; exit 1; }; done
fi

VERSION="$(date +%Y%m%d)-$(git rev-parse --short HEAD)"
git diff --quiet HEAD -- src assets Makefile Makefile.dc Makefile.pce Makefile.win Makefile.saturn tools || VERSION="$VERSION-dirty"
OUT="$ROOT/release"
mkdir -p "$OUT"

# ---------------------------------------------------------------- Linux
package_linux() {
    echo "== Linux AppImage build"
    local appimagetool="${APPIMAGETOOL:-appimagetool}"
    command -v "$appimagetool" >/dev/null 2>&1 || {
        echo "appimagetool not found; install AppImageKit or set APPIMAGETOOL to its executable" >&2
        exit 1
    }
    make -j"$(nproc)"
    local stage="$OUT/linux/AppDir"
    local bin="$stage/usr/bin/saber_rider" lib="$stage/usr/lib"
    local share="$stage/usr/share/saber-rider"
    rm -rf "$stage"
    mkdir -p "$stage/usr/bin" "$lib" "$share/data" "$stage/usr/share/doc/saber-rider"

    strip --strip-debug -o "$bin" saber_rider
    cp -r assets "$share/"
    for p in "${PACKS[@]}"; do cp "$DATA/$p" "$share/data/"; done

    # Bundle everything the binary links except glibc and the libraries that must match the user's desktop
    # (X11 / xcb / Wayland / GL / DRM, ALSA / PulseAudio / D-Bus and the libraries PulseAudio pulls in).
    local keep_exact='^(ld-linux-x86-64|linux-vdso|libc|libm|libdl|libpthread|librt|libresolv|libutil|libXau|libXdmcp|libasound|libdbus-1|libffi|libsndfile|libFLAC|libmpg123|libasyncns|libsystemd|libcap|libgbm)\.so'
    local keep_family='^(libX11|libxcb|libwayland|libGL|libEGL|libOpenGL|libdrm|libpulse)'
    ldd saber_rider | awk '$2 == "=>" && $3 ~ /^\// { print $1, $3 }' | while read -r name path; do
        name="${name##*/}"                 # the loader is listed by path (/lib64/ld-linux-x86-64.so.2 => ...)
        [[ "$name" =~ $keep_exact || "$name" =~ $keep_family ]] && continue
        cp -L "$path" "$lib/$name"
    done
    if LD_LIBRARY_PATH="$lib" ldd "$bin" | grep -q "not found"; then
        LD_LIBRARY_PATH="$lib" ldd "$bin" | grep "not found" >&2; exit 1
    fi
    local glibc
    glibc="$(objdump -T "$bin" "$lib"/*.so* 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1 | cut -d_ -f2)"

    cat > "$stage/AppRun" <<'EOF'
#!/bin/sh
# AppImage runtime sets APPDIR; readlink also supports direct AppRun use while developing the AppDir.
APPDIR="${APPDIR:-$(dirname "$(readlink -f "$0")")}"
export LD_LIBRARY_PATH="$APPDIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export SABER_ASSETS="$APPDIR/usr/share/saber-rider/assets"
exec "$APPDIR/usr/bin/saber_rider" "$APPDIR/usr/share/saber-rider/data" "$@"
EOF
    chmod +x "$stage/AppRun" "$bin"

    cat > "$stage/saber-rider.desktop" <<'EOF'
[Desktop Entry]
Name=Saber Rider and the Star Sheriffs
Exec=AppRun
Icon=saber_rider
Type=Application
Categories=Game;ArcadeGame;
Terminal=false
EOF
    cp SaberRider/icon.png "$stage/saber_rider.png"

    cat > "$stage/usr/share/doc/saber-rider/README.txt" <<EOF
Saber Rider and the Star Sheriffs - demo reconstruction, Linux x86_64 AppImage ($VERSION)

Run the AppImage (or pass --level N to start on stage N, 1-7).

Needs glibc $glibc or newer, and an X11 or Wayland desktop with ALSA or PulseAudio/PipeWire sound.
The AppImage bundles the game, SDL3, FFmpeg, Vorbis, assets and the demo's .pck packs.

Controls: arrows move, W/A jump, S/D shoot, hold Q/E to aim (8 directions), X/F power attack,
Enter starts / pauses, Alt+Enter toggles fullscreen. Down+jump slides; down+jump on a platform drops through.
Gamepad: d-pad/stick, South jump, East/West shoot, North power attack, shoulders aim, Start pause.
OPTIONS sets the screen (fullscreen / window size), ratio and filter; OPTIONS > CONTROLS remaps the keyboard
and the gamepad (saved in ~/.local/share/SaberRider/SaberRider/controls.cfg).
EOF
    local appimage="$OUT/saber_rider-linux-x86_64-$VERSION.AppImage"
    rm -f "$appimage"
    local runtime_args=()
    if [ -n "${APPIMAGE_RUNTIME:-}" ]; then runtime_args=(--runtime-file "$APPIMAGE_RUNTIME"); fi
    APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 "$appimagetool" "${runtime_args[@]}" "$stage" "$appimage"
    chmod +x "$appimage"
    echo "-> $appimage ($(du -h "$appimage" | cut -f1))"
}

# ---------------------------------------------------------------- Windows
package_win() {
    echo "== Windows build"
    make -f Makefile.win -j"$(nproc)" DATA="$DATA"
    local stage="$OUT/windows" dir="$OUT/windows/SaberRider"
    rm -rf "$stage"; mkdir -p "$dir/data"
    "${CROSS:-x86_64-w64-mingw32-}strip" -o "$dir/saber_rider.exe" build/win/saber_rider.exe
    cp -r assets "$dir/assets"
    sed 's/$/\r/' tools/win/README.txt > "$dir/README.txt"
    for p in "${PACKS[@]}"; do cp "$DATA/$p" "$dir/data/"; done
    local zipf="$OUT/saber_rider-windows-x86_64-$VERSION.zip"
    rm -f "$zipf"
    (cd "$stage" && zip -qr9 "$zipf" SaberRider)
    echo "-> $zipf ($(du -h "$zipf" | cut -f1))"
}

# ---------------------------------------------------------------- Dreamcast
package_dc() {
    echo "== Dreamcast build"
    local env_sh="${KOS_ENV:-/opt/toolchains/dc/kos/environ.sh}"
    if [ -f build/dc/stage/saber.env ]; then
        echo "build/dc/stage/saber.env holds debug switches; remove it before a release" >&2; exit 1
    fi
    local target=rebake                   # textures, samples and files baked again; the converted music and videos kept
    [ $full_disc = 1 ] || [ ! -d build/dc/stage/music ] && target=disc
    (   set +u                             # KOS's environ scripts read variables that may be unset
        # shellcheck disable=SC1090
        source "$env_sh"
        set -u
        make -f Makefile.dc -j"$(nproc)"
        make -f Makefile.dc "$target" DATA="$DATA"
    )
    local stage="$OUT/dreamcast" dir="$OUT/dreamcast/SaberRider-Dreamcast"
    rm -rf "$stage"; mkdir -p "$dir"
    cp build/dc/saber_rider.cdi "$dir/"
    cp build/dc/'Front Cover 256x256.png' "$dir/"
    cat > "$dir/README.txt" <<EOF
Saber Rider and the Star Sheriffs - demo reconstruction, Sega Dreamcast ($VERSION)

saber_rider.cdi is a self-booting disc image (MIL-CD): burn it to a CD-R (DiscJuggler / ImgBurn / cdrecord),
load it on an ODE (GDEMU, MODE, USB-GDROM), or run it in an emulator (Flycast, Redream).

Controls: D-pad or stick moves, A jumps, B/X shoots, Y uses the power attack, triggers aim, Start pauses.
A+B+X+Y+Start resets to the console menu.

OPTIONS > SCREEN: 640x480 (default) or 320x240 (always 4:3) on every cable; with a VGA cable also
832x480 60 Hz (CVT reduced blanking, a 416x240 wide screen doubled; needs a display or scaler that
accepts it, e.g. an OSSC or most LCD TVs).
EOF
    local zipf="$OUT/saber_rider-dreamcast-$VERSION.zip"
    rm -f "$zipf"
    (cd "$stage" && zip -qr9 "$zipf" SaberRider-Dreamcast)
    echo "-> $zipf ($(du -h "$zipf" | cut -f1))"
}

# ---------------------------------------------------------------- Saturn
package_saturn() {
    echo "== Saturn build"
    local env_sh="${YAUL_ENV:-$HOME/.yaul.env}"
    [ -f "$env_sh" ] || { echo "Yaul environment not found: $env_sh" >&2; exit 1; }
    rm -rf build/saturn obj-saturn
    (   source "$env_sh"
        make -j"$(nproc)" -f Makefile.saturn disc "DATA=$DATA" RENDER=vdp
    )
    local stage="$OUT/saturn" dir="$OUT/saturn/SaberRider-Saturn"
    rm -rf "$stage"; mkdir -p "$dir"
    cp build/saturn/saber_rider.cue build/saturn/saber_rider.bin "$dir/"
    cat > "$dir/README.txt" <<EOF
Saber Rider and the Star Sheriffs - demo reconstruction, Sega Saturn ($VERSION)

saber_rider.cue + saber_rider.bin is one disc image (data track and CD-DA tracks in a single bin, like a
retail Saturn disc); burn it (ImgBurn / cdrecord with the cue sheet), load it on an ODE (Satiator,
action-replay-based, etc.), or run it in an emulator (Mednafen, SSF, Kronos).

Controls: D-pad moves, A/B/C shoot / power attack / jump per layout, shoulders aim, Start pauses.
EOF
    local zipf="$OUT/saber_rider-saturn-$VERSION.zip"
    rm -f "$zipf"
    (cd "$stage" && zip -qr9 "$zipf" SaberRider-Saturn)
    echo "-> $zipf ($(du -h "$zipf" | cut -f1))"
}

# ---------------------------------------------------------------- PC Engine
package_pce() {
    echo "== PC Engine Arcade CD-ROM² build"
    make -f Makefile.pce -j"$(nproc)" all
    local cue=build/pce/saber_rider.cue
    [ -f "$cue" ] || { echo "no $cue (the disc build did not run)" >&2; exit 1; }

    local stage="$OUT/pce" dir="$OUT/pce/SaberRider-PCEngine-CDROM2"
    rm -rf "$stage"; mkdir -p "$dir"
    cp "$cue" "$dir/"
    # The cue sheet is the manifest: the data ISO plus one file per CD-DA track. Copy exactly what it names.
    local f
    for f in $(sed -n 's/^FILE "\([^"]*\)".*/\1/p' "$cue"); do
        [ -f "build/pce/$f" ] || { echo "cue references missing build/pce/$f" >&2; exit 1; }
        cp "build/pce/$f" "$dir/"
    done
    cp build/pce/runtime.json "$dir/" 2>/dev/null || true
    cat > "$dir/README.txt" <<EOF
Saber Rider and the Star Sheriffs - demo reconstruction, PC Engine / TurboGrafx-16 Arcade CD-ROM² ($VERSION)

saber_rider.cue is the disc image: one Mode 1 data track (saber_rider.iso, the game and its assets) plus
57 CD-DA music tracks (music00.bin ... music17.bin, music_end.bin and their _m and _l banks). The cue sheet
names every file, so keep them all beside it in the same folder.

HARDWARE REQUIRED - this is an Arcade CD-ROM² title, not a plain Super CD-ROM² one

  1. A Super CD-ROM² drive: the Japanese Super CD-ROM² unit, or the Super CD-ROM² built into a US TurboDuo,
     CoreGrafx or SuperGrafx.
  2. The Arcade Card HuCard stacked on that drive: Arcade Card PRO for the Japanese Super CD-ROM², Arcade Card
     DUO for the US TurboDuo (the US machines need the HuCard converter as well).

Both are needed, and the CD system BIOS must be v3.0 or newer. The Arcade Card HuCard supplies the 2 MiB of
Arcade RAM the game keeps its graphics, maps and sample archives in. A standard Super CD-ROM² HuCard will NOT
work: the game finds no Arcade RAM and stops on a "SUPER CD V3 + ARCADE CARD REQUIRED" screen. The original
CD-ROM² drive and the US TurboGrafx-CD will not boot it either - both run a v2.0 CD BIOS, and the disc refuses
anything older than v3.0.

The Analogue Duo, the FPGA PC Engine / TurboGrafx-16 clone, plays it as well: its CD-ROM² mode covers Arcade
CD-ROM², so leave that option on Arcade (or let the Duo auto-detect) and restart the disc after changing it.
It reads burnt CD-Rs, so a CD-R of this cue is all it needs.

Emulators do not need the Arcade BIOS or a real Arcade Card. Any PC Engine emulator will do as long as you
load the Super CD-ROM System BIOS (Japan v3.0 or newer is what this build is tested against) and turn
Arcade Card / arcade RAM emulation on - for example mednafen-pce with the Super CD-ROM BIOS and
pce.arcadecard 1 (mednafen-pce-headless takes the same setting in its pce.cfg; Mednafen enables it by
default). In RetroArch's Beetle PCE core it is the arcade-card toggle in the core options. Leave the accurate
hardware limits in place: sprite limits on, no unlimited SAT. With Arcade Card emulation off the game sits on
the same REQUIRED screen instead of starting.

Then burn the cue (ImgBurn / cdrecord / cdrdao with the cue sheet), run the cue in an emulator, or load the
disc on an ODE or flash cart behind the Arcade Card.

Controls: D-pad moves, II jumps, I shoots; down crouches, down + II drops through a one-way platform or
slides; hold Select with a direction to aim. Hero power is button III on a 6-button pad (6-button mode),
or a quick tap of Select (under 20 frames, no direction held) on a 2-button pad.
In the race: up gas, down brake, left/right steer, II turbo or ram, I fires in pursuit.
Ramrod: left/right aim, up/down change range, I guns, II close punch. Dialogs advance with I or II.
EOF
    local zipf="$OUT/saber_rider-pce-cdrom2-$VERSION.zip"
    rm -f "$zipf"
    (cd "$stage" && zip -qr9 "$zipf" SaberRider-PCEngine-CDROM2)
    echo "-> $zipf ($(du -h "$zipf" | cut -f1))"
}

[ $want_linux = 1 ] && package_linux
[ $want_win = 1 ] && package_win
[ $want_dc = 1 ] && package_dc
[ $want_sat = 1 ] && package_saturn
[ $want_pce = 1 ] && package_pce
ls -la "$OUT"/*.zip
