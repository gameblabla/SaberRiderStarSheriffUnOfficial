# Debug level selection

Build the SGX disc with `make -f Makefile.pce SGX=1 DEBUG=1 all`, or the
single-VDC PCE disc with `make -f Makefile.pce DEBUG=1 all`.

In Options, move down to LEVEL and use left/right to choose 1–7. The value
wraps at either end and starts at 1. Return to the title, start the game and
choose a hero: the selected level loads through its normal mission startup.
The choice persists for the running session, including new games after game
over or credits. Continues still restart the current level/phase.

`DEBUG` defaults to 0; the level row and stored selection are compiled out
unless `DEBUG=1`. This option is independent of `RETAIL`, so it works with the
normal campaign build. Changing build flags automatically rebuilds the disc.

Native regression check after building:

```
python3 tools/pce/test_debug_level_select.py --out build/sgx --sgx
```

For PCE, use `--out build/pce` without `--sgx`.

Validation: both PCE and SGX `DEBUG=1` discs built and passed the native
regression above, covering all seven selectable values, wraparound, the
visible level digits, returning to Options, starting level 3, and starting
level 3 again after game over. The SGX Options capture was visually reviewed.
Default SGX compilation and ELF checks also passed, with `start_level` and
`level_text` absent from the linked symbols. `git diff --check` passed.
The full campaign/regression suite was not repeated for this menu change.
