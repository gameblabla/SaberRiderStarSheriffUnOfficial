# SGX added horse left-edge exit

The shared enemy update retired horse actors at `actor X < camera - 80`.
The full-size added horse starts at `actor X - camera + 40`, so it vanished
at X = -41 while most of its 128-pixel width remained onscreen.

SGX gameplay now keeps horse actors through `camera - 167`. The added horse
retains its final column at X = -127 and is fully culled at X = -128.
Existing per-column renderer culling already discards each 32-pixel column
only when it is fully offscreen. Other actor types and the PCE cutoff retain
their existing behavior.

`test_sgx_herd_exit.py` calls the compiled actor update and renderer across
138 exit positions, checks all surviving column coordinates, and verifies
that the convoy remains active until the added horse exits. It fails against
the previous build at X = -41 and passes with the fix. It is included in
`make -f Makefile.pce test-sgx`.

Both retail builds and their ELF/bank checks pass. All three native convoy
regressions pass in both PCE and SGX. Validation uses the accurate headless
emulator with hardware sprite limits; physical hardware was not tested.
