#!/usr/bin/env python3
"""Host lifecycle regression for the actual Saturn movie backend."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='saturn-movie-test-') as tmp:
    work = Path(tmp)
    (work / 'yaul.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
extern void *hw_memalign(size_t,size_t);
#define __packed __attribute__((packed))
#define __aligned(n) __attribute__((aligned(n)))
#define __unused __attribute__((unused))
typedef struct { uint32_t starting_fad, size; } cdfs_filelist_entry_t;
typedef enum {VDP2_TVMD_TV_STANDARD_NTSC, VDP2_TVMD_TV_STANDARD_PAL} vdp2_tvmd_tv_standard_t;
extern vdp2_tvmd_tv_standard_t vdp2_tvmd_tv_standard_get(void);
extern void vdp1_sync_wait(void);
#define dbgio_printf(...) ((void)0)
''')
    (work / 'gamemath.h').write_text('')
    exe = work / 'movie_lifecycle'
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-DREAL_FIXED', '-DFX_NO_FLOAT', '-DPLAT_SATURN',
                    '-Wno-pointer-to-int-cast', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-I', str(work), '-I', str(ROOT / 'third_party/sv24'),
                    str(ROOT / 'tools/saturn/tests/movie_lifecycle.c'), str(ROOT / 'third_party/sv24/svm.c'), '-o', str(exe)], check=True)
    subprocess.run([exe], check=True)
