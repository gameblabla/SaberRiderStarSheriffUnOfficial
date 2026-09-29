#!/usr/bin/env python3
"""Exercise the actual Saturn CD reader against delayed/failing firmware replies."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='saturn-cd-test-') as tmp:
    work = Path(tmp)
    (work / 'yaul.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef uint32_t fad_t;
typedef struct { int type; char name[16]; fad_t starting_fad; uint32_t size; } cdfs_filelist_entry_t;
typedef struct { cdfs_filelist_entry_t *entries; uint32_t entries_count; } cdfs_filelist_t;
typedef struct { uint8_t cd_status; fad_t fad; } cd_block_status_t;
#define CDFS_ENTRY_TYPE_FILE 1
cdfs_filelist_entry_t *cdfs_entries_alloc(int n);
void cdfs_config_default_set(void);
void cdfs_filelist_init(cdfs_filelist_t *, cdfs_filelist_entry_t *, int);
void cdfs_filelist_root_read(cdfs_filelist_t *);
uint8_t cpu_intc_mask_get(void);
void cpu_intc_mask_set(uint8_t);
''')
    exe = work / 'cd_loading'
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra',
                    '-Wno-unused-parameter',
                    '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                    '-I', str(work), str(ROOT / 'tools/saturn/tests/cd_loading.c'),
                    '-o', str(exe)], check=True)
    subprocess.run([exe], check=True, timeout=30)
