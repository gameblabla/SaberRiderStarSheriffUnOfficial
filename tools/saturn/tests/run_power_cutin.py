#!/usr/bin/env python3
"""Check Saturn first-use movies and resident cut-in artwork for all heroes."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='saturn-power-test-') as tmp:
    exe = Path(tmp) / 'power_cutin'
    subprocess.run(['cc', '-std=gnu11', '-O1', '-ffunction-sections', '-fdata-sections',
                    '-DPLAT_SATURN', '-DREAL_FIXED', '-DFX_NO_FLOAT', '-I', str(ROOT / 'src'),
                    str(ROOT / 'tools/saturn/tests/power_cutin.c'), '-Wl,--gc-sections',
                    '-o', str(exe)], check=True)
    subprocess.run([exe], check=True)
