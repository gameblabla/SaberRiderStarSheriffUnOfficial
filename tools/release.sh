#!/usr/bin/env bash
# Package a release: three separate zips in release/
#   saber_rider-linux-x86_64-<version>.zip  the SDL3 build, its bundled libraries, assets/ and the demo's data/*.pck
#   saber_rider-windows-x86_64-<version>.zip  the MinGW-w64 static build (no DLLs to ship), assets/ and the demo's data/*.pck
#   saber_rider-dreamcast-<version>.zip     the self-booting CDI (the packs are on the disc)
#
#   tools/release.sh [--linux] [--win] [--dc] [--full-disc] [DATA_DIR]
#     --linux / --win / --dc   only those packages (default: all)
#     --full-disc      rebuild the whole Dreamcast disc (music and FMV conversion too) instead of re-baking build/dc/stage
#     DATA_DIR         the demo's data/ folder with the .pck packs (default SaberRider/data, the copy in this repo)
# The Dreamcast part sources $KOS_ENV (default /opt/toolchains/dc/kos/environ.sh).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

want_linux=0 want_win=0 want_dc=0 full_disc=0 DATA=""
for a in "$@"; do
    case "$a" in
        --linux) want_linux=1 ;;
        --win|--windows) want_win=1 ;;
        --dc) want_dc=1 ;;
        --full-disc) full_disc=1 ;;
        -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
        -*) echo "unknown option $a" >&2; exit 2 ;;
        *) DATA="$a" ;;
    esac
done
[ $want_linux = 0 ] && [ $want_win = 0 ] && [ $want_dc = 0 ] && want_linux=1 want_win=1 want_dc=1
DATA="$(cd "${DATA:-$ROOT/SaberRider/data}" && pwd)"
PACKS=(pack.pck common.pck levels.pck menu.pck level1.pck video.pck)
for p in "${PACKS[@]}"; do [ -f "$DATA/$p" ] || { echo "missing $DATA/$p (pass the demo's data/ folder)" >&2; exit 1; }; done

VERSION="$(date +%Y%m%d)-$(git rev-parse --short HEAD)"
git diff --quiet HEAD -- src assets Makefile Makefile.dc Makefile.win tools || VERSION="$VERSION-dirty"
OUT="$ROOT/release"
mkdir -p "$OUT"

# ---------------------------------------------------------------- Linux
package_linux() {
    echo "== Linux build"
    make -j"$(nproc)"
    local stage="$OUT/linux" dir="$OUT/linux/SaberRider"
    rm -rf "$stage"; mkdir -p "$dir/lib" "$dir/data"

    strip --strip-debug -o "$dir/saber_rider" saber_rider
    cp -r assets "$dir/assets"
    for p in "${PACKS[@]}"; do cp "$DATA/$p" "$dir/data/"; done

    # Bundle everything the binary links except glibc and the libraries that must match the user's desktop
    # (X11 / xcb / Wayland / GL / DRM, ALSA / PulseAudio / D-Bus and the libraries PulseAudio pulls in).
    local keep_exact='^(ld-linux-x86-64|linux-vdso|libc|libm|libdl|libpthread|librt|libresolv|libutil|libXau|libXdmcp|libasound|libdbus-1|libffi|libsndfile|libFLAC|libmpg123|libasyncns|libsystemd|libcap|libgbm)\.so'
    local keep_family='^(libX11|libxcb|libwayland|libGL|libEGL|libOpenGL|libdrm|libpulse)'
    ldd saber_rider | awk '$2 == "=>" && $3 ~ /^\// { print $1, $3 }' | while read -r name path; do
        name="${name##*/}"                 # the loader is listed by path (/lib64/ld-linux-x86-64.so.2 => ...)
        [[ "$name" =~ $keep_exact || "$name" =~ $keep_family ]] && continue
        cp -L "$path" "$dir/lib/$name"
    done
    if LD_LIBRARY_PATH="$dir/lib" ldd "$dir/saber_rider" | grep -q "not found"; then
        LD_LIBRARY_PATH="$dir/lib" ldd "$dir/saber_rider" | grep "not found" >&2; exit 1
    fi
    local glibc
    glibc="$(objdump -T "$dir/saber_rider" "$dir"/lib/*.so* 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1 | cut -d_ -f2)"

    cat > "$dir/saber_rider.sh" <<'EOF'
#!/bin/sh
# Saber Rider launcher: the bundled libraries in lib/, the demo packs in data/. Extra arguments go to the game
# (--level N skips the front end, e.g. ./saber_rider.sh --level 2).
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here" || exit 1
LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" exec "$here/saber_rider" "$here/data" "$@"
EOF
    chmod +x "$dir/saber_rider.sh" "$dir/saber_rider"

    cat > "$dir/README.txt" <<EOF
Saber Rider and the Star Sheriffs - demo reconstruction, Linux x86_64 ($VERSION)

Run ./saber_rider.sh (or: ./saber_rider.sh --level N to start on stage N, 1-6).

Needs glibc $glibc or newer, and an X11 or Wayland desktop with ALSA or PulseAudio/PipeWire sound.
The other libraries (SDL3, FFmpeg, Vorbis, ...) are bundled in lib/.
data/ holds the original demo's .pck packs, which the game reads directly.

Controls: arrows move, W/A jump, S/D shoot, hold Q/E to aim (8 directions), X/F power attack,
Enter starts / pauses, Alt+Enter toggles fullscreen. Down+jump slides; down+jump on a platform drops through.
Gamepad: d-pad/stick, South jump, East/West shoot, North power attack, shoulders aim, Start pause.
OPTIONS sets the screen (fullscreen / window size), ratio and filter; OPTIONS > CONTROLS remaps the keyboard
and the gamepad (saved in ~/.local/share/SaberRider/SaberRider/controls.cfg).
EOF
    local zipf="$OUT/saber_rider-linux-x86_64-$VERSION.zip"
    rm -f "$zipf"
    (cd "$stage" && zip -qr9 "$zipf" SaberRider)
    echo "-> $zipf ($(du -h "$zipf" | cut -f1))"
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

[ $want_linux = 1 ] && package_linux
[ $want_win = 1 ] && package_win
[ $want_dc = 1 ] && package_dc
ls -la "$OUT"/*.zip
