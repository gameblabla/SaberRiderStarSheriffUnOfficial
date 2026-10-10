# SGX sky, power restoration and arithmetic

Starting commit: `fb8199a`, with the preceding sprite lifecycle changes still
in the worktree. Those changes are retained and documented separately in
`SGX_SPRITE_LIFECYCLE_20261009.md`.

The report is `sgx_issues_last2.txt`.

## Rendering changes

Stage 4 now composites the source mountains over their sky before palette
fitting and tile budgeting. One VDC1 panorama scrolls at the foreground camera
speed (Q8 speed 256), with no sky raster split. Its wrapped tile-budget window
includes both sky and mountains plus replacement-column slack. Previously the
two bands had separate windows and budgets despite sharing one pattern cache.

The power cut-in's VDC0 restore treated every SGX platform stage except 1 and 3
as an empty sprite-only foreground. Stages 4 and 5 consequently lost the BAT
cells under the cut-in. All platform stages now restore their cached cells
using the SGX foreground map and SGX palette 15.

Stage 3 retains an uploaded moon phase independently on each pattern page.
Moving the sky window without changing that page's phase no longer uploads
the same 5,408 bytes again. The undecorated BAT shadow still restores each page
before decorating it, protecting against old moon cells.

## Performance and arithmetic

`fast_math.S` supplies the LLVM-MOS compiler ABI through linker wrappers. The
original SDK multiply/divide/remainder implementations are absent from the SGX
ELF. Byte products use a page-aligned quarter-square table; word products use
up to three byte products. The common word-by-byte archive-offset calculation
uses two byte products and retains its complete 24-bit result. General long
products retain the low 32 bits. Unsigned byte/word division normalizes the
divisor, sharing quotient and remainder work. Long division skips leading zero
dividend bits. Signed word division rounds toward zero.

Math code remains in always-mapped bank $68. The immutable multiplication table
uses 1,024 bytes in bank $80; its reader temporarily maps that bank through
MPR6 and restores the caller's mapping. IRQs remain enabled and the existing
IRQ handlers preserve MPR6. Page-aligned absolute indexed reads avoid building
indirect pointers for each product. The race's signed-byte road multiply uses
the same unsigned product with exact high-byte sign correction.
The resident bank uses 7,559 bytes instead of 8,124,
saving 565 bytes there. The table adds data outside the resident bank; this is
not a claim that the whole application shrank.

The race computes its 256 exact segment reciprocals once when loading the
track. The idle platform column-cache bytes 1152–1663 store this table; the
road pages at 0–767 and knots at 1024–1087 remain separate. Per-frame projection
now reads the reciprocal rather than dividing when the nearest segment changes.

Platform sprite allocation indexes the VCE's 16 palette identities instead of
duplicating them across 48 pose slots. Existing generation pins still protect
live palettes. Every sky load clears those identities, including a restart of
the same stage after the menus have changed the VCE.
When retiring an ordinary pose, allocation clears only its recorded contiguous
pattern block on each VDC. Previously every allocation scanned all 54 pages
several times, including completely empty slots. The ownership checks remain,
and owner 48 retains the full scan because it also represents horse and hull
reservations. Displayed-generation pins and placement rules remain the same.

The ELF checker rejects generic SDK arithmetic returning to an SGX build and
guards the new code/table bank placements.

## Validation

WIP at the user's requested stop. Final profiles and arithmetic checks below
passed; the final campaign and remaining rendering/restoration reruns are
incomplete. Earlier candidate regression passes do not establish final coverage.

Binary tests use Luna-6 (`gpt-6-luna`) at xhigh with the accurate headless
Mednafen SGX core and hardware sprite limits. Seeded emulator checks and
profiles are not physical-console tests.

The matching fixtures measured:

| Fixture | Baseline | Final |
| --- | --- | --- |
| Stage 1 walking/firing combat, 180 video frames | 162 presentations, 54.0 fps | 165 presentations, 55.0 fps |
| Stage 3 walking/firing combat, 180 video frames | 178 presentations, 59.33 fps | 180 presentations, 60.0 fps |
| Stage 2 road autopilot, 12 seconds | 19.42 road commits/second | 20.75 road commits/second |

The level-2 profile attributed 9.1% of cycles to the generic word multiply,
which is absent from the final ELF. The signed-byte road multiply fell from
8.2% to 3.81% of cycles. Road cadence improved 6.9%; essential sprite-budget
overflow was two events in both baseline and final timed runs.

In stage 1's first 30 video frames, allocator work fell from 13.05% to 8.19%
of cycles, including the new block-release helper. The final six presentation
windows were 17, 28, 30, 30, 30, 30. Cold pose loading still causes transient
drops; this does not establish universal 60 fps. Stage 3 sprite uploads fell
from 6,528 to 3,456 bytes in the matched combat fixture. Neither fixture covers
every camera position, enemy mix or moon transition.

`test_sgx_fast_math.py` executes the actual linked routines on the emulated
HuC6280: all 65,536 byte multiply pairs, all 65,280 nonzero-divisor byte
quotient/remainder pairs, 248 word vectors and 235 long vectors. Boundary and
seeded random vectors include the archive-offset fast path, signed division
and wide divisors. Every call preserves MPR6 and the ABI's callee-saved
imaginary registers rc16–rc31. The exhaustive division test caught and fixed
a normalization flag error before final validation.

`test_race_math.py --sgx` also passes all 65,536 signed-byte road products and
checks all 256 precomputed track reciprocals against their exact values.
