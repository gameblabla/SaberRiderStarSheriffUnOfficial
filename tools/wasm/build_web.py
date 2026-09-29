#!/usr/bin/env python3
"""Stage the WASM port into one static directory a browser can load.

The module is built by make -f Makefile.wasm; this puts the page next to it and lays out the data the game reads:

  build/wasm/web/
    index.html  style.css  saber-wasm.js  saber-audio-worklet.js
    saber_rider.wasm
    data/            the demo's .pck packs, copied (the page fetches them and hands the bytes to the module)
    assets/          our own PNGs and wavs, copied
    manifest.json    the file list with sizes, so the page can show a progress bar and fetch in a sensible order

The packs are the demo's own data and are not in the repository, so --data points at them. assets/ is ours and
could be linked instead of copied, but a static directory that can be served from anywhere (or zipped and put on
a CDN) is worth the 21 MB, and it keeps the page free of any build-time path assumptions.

The page decodes the PNGs itself (the browser has a decoder, and a from-scratch inflate in the module would be
another thousand lines to get wrong), so the PNGs are staged as they are and turned into RGBA on the way in.

Two builds come out of the same sources:

  (default)          the developer build: a side panel with the level select, the SABER_* debug switches, the
                      status read-outs and the log, served on a port.
  --redist           a distributable one: a top-right button opening a controls menu, and none of the above.
                      Everything between <!-- redist-off --> and <!-- redist-on --> is deleted from index.html,
                      build-flags.js says so, and the module is expected to be the redist build (make -f
                      Makefile.wasm redist), which is compiled without the debug exports as well.

--zip writes the staged directory as one archive, which is what a download page wants: everything the game
reads is inside it and nothing is fetched from anywhere else.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# the packs the game opens, in app.c's order. video.pck is deliberately absent: video_wasm.c is the stub, the
# front end skips an intro it cannot open, and it is 95 MB of the demo's data that nothing here decodes.
PACKS = ('pack.pck', 'common.pck', 'levels.pck', 'menu.pck', 'level1.pck')

# the load order that gets to a title screen soonest: the font and the menu art are in pack.pck / menu.pck, and
# the rest fills in behind the first frames
ASSET_LATE = ('forest/', 'lab/', 'power/', 'stage3/')


# the blocks in index.html that only the developer build has. Anything between a redist-off marker and the
# matching redist-on marker is removed for a redist build, so the level select, the status read-outs, the FPS
# option, the debug switches and the log console are not shipped as dead markup.
REDIST_BLOCK = re.compile(r'[ \t]*<!--\s*redist-off[^>]*-->.*?<!--\s*redist-on[^>]*-->\n?', re.S)


def strip_redist(html: str) -> str:
    out, n = REDIST_BLOCK.subn('', html)
    return out


def write_build_flags(out: Path, redist: bool, port: int) -> None:
    label = 'redistributable build' if redist else 'developer build'
    (out / 'build-flags.js').write_text(
        '/* Written by tools/wasm/build_web.py for this build. The page reads window.SABER_BUILD.redist and lays\n'
        ' * itself out from it: on a redistributable build the settings are a menu under the top-right button and\n'
        ' * there is no level select, no debug switches and no log console. */\n'
        f'window.SABER_BUILD = {{ redist: {"true" if redist else "false"}, label: {label!r}, port: {port} }};\n')


def copy_tree(src: Path, dst: Path) -> tuple[int, int]:
    """copy a directory, returning (files, bytes)"""
    files = bytes_ = 0
    for path in sorted(src.rglob('*')):
        if not path.is_file():
            continue
        rel = path.relative_to(src)
        # the PNGs the page decodes, the wavs it plays, and the text files the game parses (the .txt atlases and
        # the .lvl level files: forest/forest.lvl, lab/lab.lvl). The .m4v cut-ins stay out: video_wasm.c is the
        # stub and nothing here decodes them.
        if any(path.name.endswith(ext) for ext in ('.png', '.wav', '.txt', '.lvl')) or rel.suffix in ('.png', '.wav', '.txt', '.lvl'):
            pass
        else:
            continue
        out = dst / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, out)
        files += 1
        bytes_ += out.stat().st_size
    return files, bytes_


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--wasm', required=True, type=Path, help='the module make -f Makefile.wasm built')
    ap.add_argument('--out', required=True, type=Path, help='the static directory to fill (build/wasm/web)')
    ap.add_argument('--data', default='SaberRider/data', type=Path, help="the demo's data packs")
    ap.add_argument('--assets', default=1, type=int, help='stage assets/ too (0 for a bring-up build: packs only)')
    ap.add_argument('--port', type=int, default=8009, help='recorded in the manifest for the dev server')
    ap.add_argument('--redist', action='store_true',
                    help='stage a redistributable build: the controls menu only, no level select, no debug switches, no log')
    ap.add_argument('--zip', type=Path, help='also write the staged directory as this zip archive')
    args = ap.parse_args()

    out = args.out
    out.mkdir(parents=True, exist_ok=True)

    # the page. index.html is rewritten for a redist build; the rest is the same file in both.
    for name in ('style.css', 'saber-wasm.js', 'saber-audio-worklet.js'):
        shutil.copy2(ROOT / 'web' / name, out / name)
    html = (ROOT / 'web' / 'index.html').read_text()
    if args.redist:
        html = strip_redist(html)
    (out / 'index.html').write_text(html)
    write_build_flags(out, args.redist, args.port)
    shutil.copy2(args.wasm, out / 'saber_rider.wasm')

    # the data packs
    data_src = args.data if args.data.is_absolute() else ROOT / args.data
    data_dst = out / 'data'
    data_dst.mkdir(parents=True, exist_ok=True)
    entries: list[dict] = []
    total = 0
    missing = []
    for name in PACKS:
        src = data_src / name
        if not src.is_file():
            missing.append(name)
            continue
        size = src.stat().st_size
        shutil.copy2(src, data_dst / name)
        entries.append({'path': f'data/{name}', 'size': size, 'kind': 'pack'})
        total += size

    # our assets: the PNGs the game loads as images, the wavs it plays, the text files it parses
    if args.assets:
        assets_src = ROOT / 'assets'
        files, bytes_ = copy_tree(assets_src, out / 'assets')
        for path in sorted((out / 'assets').rglob('*')):
            if not path.is_file():
                continue
            rel = path.relative_to(out).as_posix()
            suffix = path.suffix.lower()
            kind = 'image' if suffix == '.png' else 'sound' if suffix == '.wav' else 'text'
            entries.append({'path': rel, 'size': path.stat().st_size, 'kind': kind})
            total += path.stat().st_size
        print(f'assets: {files} files, {bytes_ / 1e6:.1f} MB')

    # the load order: the packs first (the front end cannot start without them), then the assets, the big art
    # last so the first frames are not waiting on it
    def rank(e: dict) -> tuple:
        if e['kind'] == 'pack':
            return (0, PACKS.index(Path(e['path']).name))
        if any(e['path'].startswith(f'assets/{p}') for p in ASSET_LATE):
            return (2, 0)
        return (1, 0)
    entries.sort(key=rank)

    manifest = {
        'internalWidth': 426,
        'internalHeight': 240,
        'port': args.port,
        'totalBytes': total,
        'missingPacks': missing,
        'files': entries,
        # cache-busting for the module: the page fetches the manifest first and loads the wasm with
        # ?v=<mtime>-<size>, so a hard reload is never needed to pick up a new build (a stale cached module
        # sounds like time-scrambled garbage, which is exactly the failure that must not be misdiagnosed)
        'wasmVersion': f'{int(args.wasm.stat().st_mtime)}-{args.wasm.stat().st_size}',
    }
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=1))

    print(f'staged {out} ({"redistributable" if args.redist else "developer"} build)')
    print(f'  {len([e for e in entries if e["kind"] == "pack"])} packs, '
          f'{len([e for e in entries if e["kind"] == "image"])} images, '
          f'{len([e for e in entries if e["kind"] == "sound"])} sounds, '
          f'{len([e for e in entries if e["kind"] == "text"])} text files')
    print(f'  {total / 1e6:.1f} MB total')
    if missing:
        print(f'  MISSING packs: {", ".join(missing)}  (the game will not start)')

    if args.zip:
        args.zip.parent.mkdir(parents=True, exist_ok=True)
        # the archive root is the site root: index.html has to be at the top of it, or a static host serves a
        # directory listing instead of the page
        with zipfile.ZipFile(args.zip, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for path in sorted(out.rglob('*')):
                if path.is_file():
                    z.write(path, path.relative_to(out).as_posix())
        size = args.zip.stat().st_size
        print(f'  {args.zip}  {size / 1e6:.1f} MB')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
