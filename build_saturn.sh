#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_root"

data_dir="${1:-SaberRider/data}"
yaul_env="$HOME/.yaul.env"

if [[ ! -f "$yaul_env" ]]; then
    printf 'Yaul environment not found: %s\n' "$yaul_env" >&2
    exit 1
fi
source "$yaul_env"

# These directories contain generated Saturn outputs and caches only.
rm -rf build/saturn obj-saturn

make -j8 -f Makefile.saturn disc "DATA=$data_dir" RENDER=vdp

grep -E 'omitted from Saturn build|audio budget: Stage 5 April' build/saturn/bake.log
python3 -c 'import json; d=json.load(open("build/saturn/audio_budget.json")); p=d["profiles"]["stage5_april"]; print(p["resident_bytes"], "/", d["bank_bytes"], "bytes; headroom:", p["headroom_bytes"], "reserve:", d["reserve_bytes"]); assert p["headroom_bytes"] >= d["reserve_bytes"]'

printf 'Saturn disc image: %s\n' "$repo_root/build/saturn/saber_rider.cue"
