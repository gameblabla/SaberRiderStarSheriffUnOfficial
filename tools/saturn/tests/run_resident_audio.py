#!/usr/bin/env python3
"""Run host checks against aud_sat.c and the current generated sound manifest."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='saturn-audio-test-') as tmp:
    work = Path(tmp)
    (work / 'yaul.h').write_text('void smpc_smc_sndoff_call(void);\nvoid smpc_smc_sndon_call(void);\n')
    exe = work / 'resident_audio'
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-DREAL_FIXED', '-DFX_NO_FLOAT', '-DPLAT_SATURN',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-I', str(work),
                    str(ROOT / 'tools/saturn/tests/resident_audio.c'), '-o', str(exe)], check=True)
    subprocess.run([exe, ROOT / 'build/saturn/stage/SOUNDS.BIN'], check=True)
