#!/usr/bin/env python3
"""Insert the hand-reconstructed straight-down torsos into the engine sheets.

Run after regenerating either sheet. Source pixels stay at native resolution;
the waist anchors fit both cell 22's standing legs and cells 54/55's fall legs.
Cells 46/47 are the aim/firing pair; both use the supplied reconstruction.
"""
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
CELL = 64


def main():
    for name, expected_size, position in (
        ("april", (17, 23), (24, 25)),
        ("saber", (16, 29), (24, 29)),
    ):
        source = Image.open(ROOT / f"{name}_downaiming.png").convert("RGBA")
        if source.size != expected_size:
            raise ValueError(f"{name}: expected {expected_size}, got {source.size}")
        path = ROOT / "assets" / f"{name}.png"
        sheet = Image.open(path).convert("RGBA")
        if sheet.width != 8 * CELL or sheet.height < 6 * CELL:
            raise ValueError(f"{name}: unexpected engine sheet size {sheet.size}")
        torso = Image.new("RGBA", (CELL, CELL))
        torso.paste(source, position)
        for index in (46, 47):
            sheet.paste(torso, (index % 8 * CELL, index // 8 * CELL))
        sheet.save(path)
        print(f"Updated {path.relative_to(ROOT)} cells 46/47 at {position}")


if __name__ == "__main__":
    main()
