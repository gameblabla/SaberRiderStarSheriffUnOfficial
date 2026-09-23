#!/usr/bin/env bash
# Build (if needed) and run the Saber Rider PC port reconstruction with its
# reconstructed assets (April sprite sheet + voice) against the original demo data.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"

DATA_DIR="${1:-SaberRider/data}"
shift || true

if [ ! -d build ]; then
    cmake -S . -B build -G Ninja
fi
cmake --build build

SABER_ASSETS="$PWD/assets" ./build/saber_rider "$DATA_DIR" "$@"
