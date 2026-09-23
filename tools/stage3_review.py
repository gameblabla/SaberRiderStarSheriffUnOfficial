#!/usr/bin/env python3
"""Run small Stage 3 review suites against the actual game binary.

This script only drives the engine, consumes its trace, and stitches captures;
it deliberately has no second copy of the route or boss implementation.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build" / "saber_rider"
DATA = ROOT / ".." / "SaberRider" / "data"


def fail(message: str) -> None:
    raise SystemExit(f"stage3 review: {message}")


def run_capture(out_dir: Path, name: str, script: str, steps: int, *, start: float | None = None,
                ratio: str = "wide", debug: bool = False, extra: dict[str, str] | None = None) -> Path:
    if not BINARY.is_file():
        fail(f"missing {BINARY}; build the game first")
    data_dir = Path(os.environ.get("SABER_DATA", DATA))
    if not data_dir.is_dir():
        fail(f"missing data directory {data_dir}; set SABER_DATA to the demo data directory")
    width = 320 if ratio == "4:3" else 426
    out_dir.mkdir(parents=True, exist_ok=True)
    bmp = out_dir / f"{name}.bmp"
    log_path = out_dir / f"{name}.log"
    env = os.environ.copy()
    env.update({
        "SABER_ASSETS": str(ROOT / "assets"),
        "SABER_STAGE": "3",
        "SABER_TRACE": "1",
        "SABER_SCRIPT": script,
        "SABER_SHOT": f"{bmp},-1,{steps}",
        "SABER_WINDOW": f"{width}x240",
        "SDL_AUDIO_DRIVER": "dummy",
        "SDL_AUDIODRIVER": "dummy",
    })
    if ratio == "4:3":
        env["SABER_RATIO"] = "-1"
    else:
        env.pop("SABER_RATIO", None)
    if start is not None:
        env["SABER_START"] = str(start)
    else:
        env.pop("SABER_START", None)
    if debug:
        env["SABER_DEBUG"] = "1"
    else:
        env.pop("SABER_DEBUG", None)
    if extra:
        env.update(extra)

    command = [str(BINARY), str(data_dir), "--level", "3"]
    if not env.get("DISPLAY") and shutil.which("xvfb-run"):
        command = ["xvfb-run", "-a", *command]
    try:
        result = subprocess.run(command, cwd=ROOT, env=env, text=True, capture_output=True, timeout=90)
    except subprocess.TimeoutExpired:
        fail(f"{name} exceeded 90 seconds")
    log = result.stdout + result.stderr
    log_path.write_text(log)
    if result.returncode != 0:
        fail(f"{name} exited {result.returncode}; see {log_path}")
    if not bmp.is_file():
        fail(f"{name} did not produce {bmp}")
    return log_path


def trace_lines(log_path: Path) -> list[str]:
    return [line for line in log_path.read_text().splitlines() if line.startswith("stage3 trace ")]


def player_lines(log_path: Path) -> list[str]:
    return [line for line in log_path.read_text().splitlines() if line.startswith("cam=")]


def assert_ground_clear(log_path: Path, landing_x: float) -> None:
    lines = player_lines(log_path)
    if not lines:
        fail(f"{log_path.name} has no player trace")
    last = lines[-1]
    match = re.search(r"st=(\d+).*pos=([0-9.-]+),([0-9.-]+)", last)
    if not match:
        fail(f"could not parse final player trace in {log_path}")
    if int(match.group(1)) == 9:
        fail(f"player died during {log_path.name}")
    if float(match.group(2)) < landing_x:
        fail(f"player did not clear the landing edge in {log_path.name}: {last}")


def stitch(captures: list[Path], cameras: list[int], destination: Path, width: int) -> None:
    images = [Image.open(path).convert("RGBA") for path in captures]
    if not images:
        fail("cannot stitch an empty capture set")
    if len(images) != len(cameras):
        fail("overview capture/camera count mismatch")
    if any(image.width != width for image in images):
        fail(f"overview capture width does not match logical viewport {width}")
    sheet = Image.new("RGBA", (6200, max(image.height for image in images)))
    for image, camera in zip(images, cameras):
        sheet.alpha_composite(image, (camera, 0))
    destination.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(destination)


def suite_start(root: Path, ratio: str) -> None:
    log = run_capture(root / "start", "idle", "120:", 120, ratio=ratio)
    if any("flow=ARENA_ENTRY" in line or "flow=BOSS" in line for line in trace_lines(log)):
        fail("boss activated while the hero was idle at the start")


def suite_traversal(root: Path, ratio: str) -> None:
    # These are short, real-engine fixtures at three route locations.  They
    # keep a geometry check independent from encounter damage and verify that
    # the continuous authored floor remains traversable across the whole run.
    cases = (
        ("ground-01", 1200, "20:,1:RJS,150:RS", 1312),
        ("ground-02", 2240, "20:,1:RJS,150:RS", 2352),
        ("ground-03", 3700, "20:,12:RS,1:RJS,160:RS", 3840),
    )
    for name, start, script, landing in cases:
        log = run_capture(root / "traversal", name, script, 171 if name != "ground-03" else 193,
                          start=start, ratio=ratio,
                          extra={"SABER_LIVES": "99", "SABER_STAGE3_NO_ENEMIES": "1"})
        assert_ground_clear(log, landing)


def suite_boss(root: Path, ratio: str, all_difficulties: bool, all_phases: bool) -> None:
    difficulties = (0, 1, 2) if all_difficulties else (1,)
    start = 5900 if ratio == "wide" else 6040
    phases = (1, 2, 3) if all_phases else (None,)
    for difficulty in difficulties:
        for fixture_phase in phases:
            name = f"entry-d{difficulty}" + (f"-p{fixture_phase}" if fixture_phase else "")
            extra = {"SABER_DIFFICULTY": str(difficulty)}
            if fixture_phase:
                extra["SABER_STAGE3_PHASE"] = str(fixture_phase)
            log = run_capture(root / "boss", name, "240:", 240, start=start, ratio=ratio,
                              extra=extra)
            states = trace_lines(log)
            if not any("flow=ARENA_ENTRY" in line for line in states):
                fail(f"{name} never entered the arena")
            if not any("flow=BOSS" in line for line in states):
                fail(f"{name} never reached the boss state")
            entry = next(i for i, line in enumerate(states) if "flow=ARENA_ENTRY" in line)
            boss = next(i for i, line in enumerate(states) if "flow=BOSS" in line)
            if boss <= entry:
                fail(f"{name} started combat before the entrance completed")


def suite_lifecycle(root: Path, ratio: str) -> None:
    log = run_capture(root / "lifecycle", "pause-and-death", "20:,1:P,30:,1:P,70:", 122,
                      start=150, ratio=ratio, extra={"SABER_KILL": "12", "SABER_LIVES": "3"})
    states = trace_lines(log)
    if not any("reason=player" in line and "lives=2" in line for line in states):
        fail("death/life transition was not present in the Stage 3 trace")
    start = 5900 if ratio == "wide" else 6040
    clear_log = run_capture(root / "lifecycle", "boss-clear-fixture", "1000:RUSA", 1000,
                            start=start, ratio=ratio,
                            extra={"SABER_DIFFICULTY": "0", "SABER_STAGE3_BOSSHP": "1", "SABER_LIVES": "99"})
    clear_states = trace_lines(clear_log)
    if not any("flow=DEFEAT" in line for line in clear_states) or not any("flow=CLEAR" in line for line in clear_states):
        fail("boss defeat animation/clear transition was not present in the Stage 3 trace")


def export_overview(root: Path, destination: Path, ratio: str) -> None:
    width = 320 if ratio == "4:3" else 426
    max_camera = 6200 - width
    cameras = list(range(0, max_camera + 1, width))
    if cameras[-1] != max_camera:
        cameras.append(max_camera)
    clean: list[Path] = []
    collision: list[Path] = []
    for index, camera in enumerate(cameras):
        start = camera + width * .5
        run_capture(root / "overview", f"clean-{index:02d}", "1:", 1,
                    start=start, ratio=ratio, extra={"SABER_NO_HUD": "1"})
        # Starting the hero at the center of each viewport makes the game's
        # own follow camera produce adjacent, non-overlapping engine frames.
        clean_path = root / "overview" / f"clean-{index:02d}.bmp"
        clean.append(clean_path)
        run_capture(root / "overview", f"collision-{index:02d}", "1:", 1,
                    start=start, ratio=ratio, debug=True, extra={"SABER_NO_HUD": "1"})
        collision.append(root / "overview" / f"collision-{index:02d}.bmp")
    stitch(clean, cameras, destination.with_name(destination.stem + ".png"), width)
    stitch(collision, cameras, destination.with_name(destination.stem + "-collision.png"), width)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--suite", choices=("start", "traversal", "boss", "lifecycle", "all"), default="start")
    parser.add_argument("--ratio", choices=("wide", "4:3"), default="wide")
    parser.add_argument("--all-phases", action="store_true", help="exercise the three boss attack fixtures")
    parser.add_argument("--all-difficulties", action="store_true")
    parser.add_argument("--export-overview", type=Path)
    args = parser.parse_args()
    if args.all_phases and args.suite not in ("boss", "all"):
        fail("--all-phases applies to --suite boss or all")
    root = ROOT / "artifacts" / "stage3-review" / args.ratio.replace(":", "-")
    if args.suite in ("start", "all"):
        suite_start(root, args.ratio)
    if args.suite in ("traversal", "all"):
        suite_traversal(root, args.ratio)
    if args.suite in ("boss", "all"):
        suite_boss(root, args.ratio, args.all_difficulties, args.all_phases)
    if args.suite in ("lifecycle", "all"):
        suite_lifecycle(root, args.ratio)
    if args.export_overview:
        export_overview(root, args.export_overview, args.ratio)
    print(f"stage3 review OK: suite={args.suite} ratio={args.ratio}")


if __name__ == "__main__":
    main()
